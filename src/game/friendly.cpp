#include "friendly.h"

#include <cstdint>
#include <windows.h> // GetTickCount64, for the burst-summary clock
#include <mutex>
#include <unordered_map>

#include <MinHook.h>

#include "offsets.h"
#include "../mem/safe_memory.h"
#include "../mem/hooks.h"
#include "../core/logger.h"
#include "../core/state.h"

namespace trinity::game
{
    using mem::Read16;
    using mem::Read32;
    using mem::Read64;
    using mem::Write64;

    namespace
    {
        // The two leaf trust-record setters. Both take the destination map owner
        // in rcx and the SOURCE 0x58-byte record in rdx, and copy the record into
        // the matching slot. Every trust write - gift, feed, AI, save-load, sync
        // - funnels through one of these (live-confirmed: an NPC trust write
        // broke at 0xDBE114F inside setNpc). See offsets.h (kSig_FriendlySet*).
        using FriendlySet_t = void*(__fastcall*)(void* mapOwner, void* record);
        FriendlySet_t oSetNpc = nullptr;
        FriendlySet_t oSetPet = nullptr;
        void* g_npcTarget = nullptr;
        void* g_petTarget = nullptr;

        // Last trust value we let through, per relationship. The setter writes an
        // absolute value, so we scale the increase over what we last allowed for
        // that (map, group, key) - a stable per-gift/feed multiplier. Keyed so an
        // NPC and a pet, or two different targets, never collide. First sight of
        // a key SEEDS unscaled; because the save-loader drives these same setters
        // at login, every relationship is pre-seeded there, so the first in-game
        // gift is already scaled and a loaded save is never re-scaled. Losses and
        // already-max targets pass through but keep the cache tracking the true
        // value. Self-healing: after a reload the stored value drops below our
        // cache, reads as a "loss", and reseeds.
        std::mutex g_cacheMx;
        std::unordered_map<uint64_t, int64_t> g_lastVal;

        // Burst accounting for the log. These records arrive in clumps - a warp
        // pushes every nearby NPC through in well under a second - so one line
        // per record made Trinity.log unreadable (it was ~80% of a normal
        // session). Counted here and flushed as a single line by Tick() once the
        // burst goes quiet, which still answers the only question a bug report
        // needs: did the multiplier scale anything, and did the value stay up.
        // All four are touched under g_cacheMx.
        int      g_seeded   = 0; // passed through to establish a baseline
        int      g_scaled   = 0; // multiplied
        int      g_reverted = 0; // came back LOWER than what we had written
        int      g_passed   = 0; // seen before, and we changed nothing
        uint64_t g_lastRec  = 0; // GetTickCount64() of the most recent record

        // Per-record detail, for when the summary is not enough.
        //
        // The counters above cannot tell "this record never arrived" from
        // "it arrived and we decided not to touch it" - a repeat that neither
        // scales nor reverts increments nothing and is invisible. That is
        // exactly the state a "Trust Multiplier does nothing" report leaves you
        // in, so g_passed now counts it and, while the feature is switched ON,
        // the first few records get a full line: which map, which relationship,
        // what value came in, and what we did about it.
        //
        // Hard-capped for the process lifetime rather than throttled, because
        // the failure this diagnoses shows up in the first handful of records
        // or not at all - and a cap cannot degenerate into the per-NPC spam
        // that made up most of the log before.
        constexpr int kDetailBudget = 60;
        int           g_detailLeft  = kDetailBudget;

        // Call with g_cacheMx held.
        void Detail(const char* what, uint32_t mapId, uint16_t group, uint32_t key,
                    int64_t oldVal, int64_t newVal)
        {
            if (g_detailLeft <= 0) return;
            --g_detailLeft;
            LOG("friendly/detail: %s map=%s group=%u key=%u %lld -> %lld%s",
                what, mapId ? "pet" : "npc", group, key,
                static_cast<long long>(oldVal), static_cast<long long>(newVal),
                (g_detailLeft == 0) ? " (detail budget spent; summaries only from here)" : "");
        }

        int64_t ScaleGain(int64_t oldVal, int64_t newVal, float mult)
        {
            const double scaled = static_cast<double>(oldVal) +
                                  static_cast<double>(newVal - oldVal) * static_cast<double>(mult);
            if (scaled >= static_cast<double>(kFriendly_Max)) return kFriendly_Max;
            if (scaled <= 0.0) return 0;
            return static_cast<int64_t>(scaled + 0.5);
        }

        // mapId keeps the NPC (0) and pet (1) key spaces apart in the cache.
        void ScaleRecord(void* record, uint32_t mapId)
        {
            const uintptr_t r = reinterpret_cast<uintptr_t>(record);
            if (r < kMinPointer) return;

            uint32_t key = 0;
            uint16_t group = 0;
            int64_t  newVal = 0;
            if (!Read32(r + kOff_FriendlyRec_Key, &key)) return;
            if (!Read16(r + kOff_FriendlyRec_Group, &group)) return;
            if (!Read64(r + kOff_FriendlyRec_Value, &newVal)) return;

            // Ignore obviously-out-of-range values (a wrong offset or an
            // uninitialised insert) so we never write garbage into the record.
            if (newVal < 0 || newVal > kFriendly_Max) return;

            const uint64_t base   = (static_cast<uint64_t>(mapId) << 48) |
                                    (static_cast<uint64_t>(group) << 32);
            const uint64_t ckBase = base;        // key == 0: the persisted baseline
            const uint64_t ckLive = base | key;  // this live relationship

            std::lock_guard<std::mutex> lk(g_cacheMx);
            g_lastRec = GetTickCount64();

            // key == 0 is the save-loader's persistent write - every NPC's stored
            // trust arrives this way at login, one record per group, before any
            // gameplay write. Record it as the group's baseline and NEVER scale
            // it, so a loaded save is never inflated.
            if (key == 0)
            {
                g_lastVal[ckBase] = newVal;
                return;
            }

            // Gameplay write. Old value = this relationship's last value if we've
            // seen it; else the group's persisted baseline (so the FIRST
            // interaction with an NPC - e.g. a greet, which may be the only one
            // it ever gets - still scales instead of being swallowed as a seed);
            // else 0 for a wholly new relationship.
            int64_t oldVal;
            auto itLive = g_lastVal.find(ckLive);
            if (itLive != g_lastVal.end())
            {
                oldVal = itLive->second;
            }
            else
            {
                auto itBase = g_lastVal.find(ckBase);
                if (itBase == g_lastVal.end())
                {
                    // First sight of this relationship, with no group baseline
                    // either: SEED it and pass it through untouched.
                    //
                    // This used to fall through with oldVal = 0 and scale, which
                    // is the Trust Multiplier bug. The key == 0 baseline write
                    // the comment above describes does not actually happen in TU
                    // 2.00.00 - a 2h session log holds ~250 of these and every
                    // single one reads "0 -> 100", i.e. not one baseline was ever
                    // recorded. And they were not gifts: the records arrive in
                    // bursts on area load (15 inside one second right after a
                    // warp), which is the engine pushing each nearby NPC stored
                    // trust into the map. Treating that as a gain from zero
                    // multiplied the save file itself, and every relationship in
                    // range jumped straight to the cap.
                    //
                    // Seeding makes the FIRST write we see the baseline and the
                    // second - an actual gift or greet - the thing that scales,
                    // which is what the design intended all along.
                    ++g_seeded;
                    Detail("seed  ", mapId, group, key, 0, newVal);
                    g_lastVal[ckLive] = newVal;
                    return;
                }
                oldVal = itBase->second;
            }

            const State& st = State::Get();
            const float mult = st.trustMultVal;
            const bool  on   = st.trustMult && mult > 1.0f;

            if (on && newVal > oldVal && oldVal < kFriendly_Max)
            {
                const int64_t s = ScaleGain(oldVal, newVal, mult);
                if (s != newVal && Write64(r + kOff_FriendlyRec_Value, s))
                {
                    ++g_scaled;
                    Detail("SCALED", mapId, group, key, oldVal, s);
                    return;
                }
                // A gain we recognised but did not change: either the
                // multiplier worked out to the same number, or the write was
                // refused. Worth a line - it is a different failure from never
                // seeing the gain at all.
                Detail("nowrite", mapId, group, key, oldVal, newVal);
            }
            else if (on)
            {
                // The feature is on and this relationship is already known, yet
                // there is nothing to scale. This is the line that says WHY:
                // the value did not go up (a resync or an idle re-push), or the
                // relationship is already at the cap.
                Detail(oldVal >= kFriendly_Max ? "at-max " : "no-gain",
                       mapId, group, key, oldVal, newVal);
            }

            // Falling through with a value BELOW what we last allowed means
            // something handed the true number back - a reload, or the
            // server-authority copy resyncing over our client-side write. We
            // cannot tell those apart here (unlike inventory and equipment,
            // friendly.cpp has no per-realm walk), and reseeding is the right
            // move for a reload, so the value is let through either way. It is
            // counted because a session full of reverts is the signature of the
            // second case, and that is the thing worth seeing in the log.
            if (newVal < oldVal) ++g_reverted; else ++g_passed;
            g_lastVal[ckLive] = newVal;
        }

        void* __fastcall hkSetNpc(void* mapOwner, void* record)
        {
            ScaleRecord(record, 0);
            return oSetNpc(mapOwner, record);
        }

        void* __fastcall hkSetPet(void* mapOwner, void* record)
        {
            ScaleRecord(record, 1);
            return oSetPet(mapOwner, record);
        }
    }

    bool Friendly::Install()
    {
        const bool npc = mem::InstallHookAny("friendly: NPC trust setter",
                                             { kSig_FriendlySetNpc_20001,
                                               kSig_FriendlySetNpc,
                                               kSig_FriendlySetNpc_1180 },
                                             "NPC gift Trust Multiplier disabled",
                                             &hkSetNpc, &oSetNpc, &g_npcTarget);
        const bool pet = mem::InstallHook("friendly: pet trust setter", kSig_FriendlySetPet,
                                          "pet Trust Multiplier disabled",
                                          &hkSetPet, &oSetPet, &g_petTarget);
        return npc || pet;
    }

    void Friendly::Remove()
    {
        mem::RemoveHook(&g_npcTarget);
        mem::RemoveHook(&g_petTarget);
        std::lock_guard<std::mutex> lk(g_cacheMx);
        g_lastVal.clear();
        g_seeded = g_scaled = g_reverted = g_passed = 0;
        g_lastRec = 0;
    }

    void Friendly::Tick()
    {
        // Cheap enough for a per-frame call: one unlocked read of the burst
        // clock, and the lock is only taken once a burst has actually ended.
        const uint64_t last = g_lastRec;
        if (!last || GetTickCount64() - last < kFriendlyBurst_QuietMs) return;

        int seeded = 0, scaled = 0, reverted = 0, passed = 0;
        {
            std::lock_guard<std::mutex> lk(g_cacheMx);
            if (!g_lastRec || GetTickCount64() - g_lastRec < kFriendlyBurst_QuietMs) return;
            seeded   = g_seeded;
            scaled   = g_scaled;
            reverted = g_reverted;
            passed   = g_passed;
            g_seeded = g_scaled = g_reverted = g_passed = 0;
            g_lastRec = 0;
        }
        if (seeded || scaled || reverted || passed)
            LOG("friendly: %d scaled, %d seeded, %d unchanged, %d reverted.",
                scaled, seeded, passed, reverted);
    }

    bool Friendly::Ready()
    {
        return g_npcTarget != nullptr || g_petTarget != nullptr;
    }
}
