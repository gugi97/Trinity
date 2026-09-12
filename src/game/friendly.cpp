#include "friendly.h"

#include <cstdint>
#include <cmath>   // std::pow, for the proportional trust curve
#include <windows.h> // GetTickCount64, for the burst-summary clock
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <atomic>

#include <MinHook.h>

#include "offsets.h"
#include <intrin.h>   // _ReturnAddress

#include "../mem/safe_memory.h"
#include "../mem/scanner.h"
#include "../mem/hooks.h"
#include "../core/version.h"   // TRINITY_MARKER_RESEARCH
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
        // the matching slot. These carry the writes that REPLACE a whole record:
        // gifting, save-load and network sync. They are NOT every trust write -
        // an incremental gain (greet, dialogue, petting, feeding, taming) never
        // builds a record and goes through the delta accumulator below instead.
        // See offsets.h (kSig_FriendlySet* and kSig_FriendlyAddDelta).
        using FriendlySet_t = void*(__fastcall*)(void* mapOwner, void* record);
        FriendlySet_t oSetNpc = nullptr;
        FriendlySet_t oSetPet = nullptr;
        void* g_npcTarget = nullptr;
        void* g_petTarget = nullptr;

        // The incremental path, and the one that carries greet, dialogue
        // rewards, petting, feeding and wild taming. It takes a raw delta gain
        // rather than a built record, so multiplying that delta is the whole
        // fix: the engine does its own clamp to kFriendly_Max and its own tier
        // advance on the result. No cache, no baseline, no first-sight problem
        // - unlike ScaleRecord, which can only infer a gain by diffing against
        // the last value it happened to see.
        using TrustAdd_t = void*(__fastcall*)(void* rel, uint32_t* status,
                                              uint16_t group, int64_t delta);
        TrustAdd_t oTrustAdd   = nullptr;
        void*      g_addTarget = nullptr;


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

        // Relationships whose scaled value the game handed straight back, and
        // which Trinity therefore stops scaling for the rest of the session.
        //
        // Without this the two sides fight: Trinity raises the value, the game
        // pushes its own number over the top, the cache reseeds to that number,
        // the next push reads as a fresh gain, and it starts again. A live log
        // shows exactly that shape - every summary line reading "N multiplied,
        // N reverted", for minutes, with a burst of 38 straight after a warp.
        // The multiplication was never landing for those records, so the count
        // was reporting the write rather than the result.
        //
        // g_ourValue holds only the keys Trinity itself wrote, because "came
        // back lower" alone does not mean rejection - a reload legitimately
        // lowers a value we never touched, and that must still reseed.
        std::unordered_set<uint64_t> g_ourValue;
        std::unordered_set<uint64_t> g_rejected;

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
        int      g_gains    = 0; // incremental gains multiplied (greet/feed)
        uint64_t g_lastRec  = 0; // GetTickCount64() of the most recent record

        // Proportional progression toward the cap, rather than a flat multiple
        // of the gain.
        //
        // `old + gain * mult` saturates almost immediately, which quietly ate
        // most of the slider. Measured, gift +5 from 0:
        //
        //     mult  |  1    2    3    5   10   20   50  100
        //     flat  |  5   10   15   25   50  100  100  100   <- dead from 20x
        //     this  |  5   10   14   23   40   64   92   99
        //
        // Everything from 20x up produced the identical instant max, so two
        // thirds of the range meant nothing and one gift ended the progression.
        //
        // Instead, read the multiplier as "repeat this gain, with the gap to
        // the cap shrinking each time". With f the fraction of the remaining
        // distance the base gain covers, applying it mult times leaves
        // (1 - f)^mult of that gap:
        //
        //     f      = gain / (max - old)
        //     result = max - (max - old) * (1 - f)^mult
        //
        // mult = 1 returns exactly old + gain, so 1.0x is a true no-op and the
        // game's own number is never disturbed. Above that it approaches the
        // cap asymptotically and can never pass it, so no clamp is involved in
        // the normal path and every setting stays distinguishable.
        int64_t ScaleGain(int64_t oldVal, int64_t newVal, float mult)
        {
            const int64_t gain = newVal - oldVal;

            // Losses, no-ops and 1.0x are the game's own business.
            if (gain <= 0 || mult <= 1.0f) return newVal;

            const double gap = static_cast<double>(kFriendly_Max - oldVal);
            if (gap <= 0.0) return kFriendly_Max; // already at or past the cap

            const double f = static_cast<double>(gain) / gap;
            if (f >= 1.0) return kFriendly_Max;   // this gain alone reaches it

            const double scaled =
                static_cast<double>(kFriendly_Max) -
                gap * std::pow(1.0 - f, static_cast<double>(mult));

            // Guard the arithmetic rather than trust it: never below what the
            // game itself granted, never above the cap.
            if (scaled <= static_cast<double>(newVal)) return newVal;
            if (scaled >= static_cast<double>(kFriendly_Max)) return kFriendly_Max;
            return static_cast<int64_t>(scaled + 0.5);
        }

        // Return address of the MOUNT call site (see kSig_FriendlyMountGainCall).
        // 0 means "not resolved", and then the delta path does not scale at all -
        // failing closed, because an unrecognised call site is exactly the case
        // that crashed. Set once at Install().
        uintptr_t g_mountGainRet = 0;


        // Every call site that delivers a REWARD through the setters, as
        // opposed to a load. FriendlySetNpc has six callers and the record they
        // hand over is byte-identical in all six, so the return address is the
        // only thing that separates "the player just earned this" from "the
        // save loader is restoring it". Two of the six are that loader and area
        // streaming, and scaling either one multiplies the save file itself.
        //
        // 0 entries are unresolved and match nothing, so a signature that
        // drifts costs the feature and never the save - the same failing-closed
        // rule as g_mountGainRet.
        uintptr_t g_rewardRets[2] = { 0, 0 };

        bool IsRewardCaller(uintptr_t ret)
        {
            if (!ret) return false;
            for (uintptr_t r : g_rewardRets)
                if (r && r == ret) return true;
            return false;
        }

        // mapId keeps the NPC (0) and pet (1) key spaces apart in the cache.
        // retAddr is where this setter call is returning to - see IsRewardCaller.
        void ScaleRecord(void* record, uint32_t mapId, uintptr_t retAddr)
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
                // A greet reward is a gain from whatever the NPC had, which on
                // first sight is nothing - so scale it instead of seeding. The
                // caller is the only thing that distinguishes this from the
                // area-load burst described below, and the two are otherwise
                // byte-identical.
                const bool greetReward = IsRewardCaller(retAddr);

                // Which callers actually reach this hook, and with what.
                //
                // This is what found the bug: three greets logged a return
                // address Trinity did not have in its table, while the address
                // it did have belonged to a different reward path entirely. Keep
                // it - a setter caller that moves is invisible any other way,
                // and the feature fails silently rather than loudly when it
                // does.
                //
                // Bounded, but a dozen was too few: the budget ran out in the
                // first minute of play, so when the player later tested a pet
                // the silence meant both "the pet never reaches this hook" and
                // "there was no line left to print". Those need different
                // answers, so the budget now outlasts a deliberate test and the
                // line says which setter it came through.
                //
                // RESEARCH ONLY. It prints one line per first-sight record,
                // which is the right density for hunting a moved call site and
                // the wrong one for a release log - a warp alone pushes dozens.
                // pack-release.ps1 compiles TRINITY_MARKER_RESEARCH to 0, so a
                // shipping binary does not carry these strings at all.
#if TRINITY_MARKER_RESEARCH
                {
                    static int s_seen = 0;
                    if (s_seen < 40)
                    {
                        ++s_seen;
                        LOG("friendly/who: %s first-sight key %u grp %u val %lld "
                            "from %llX%s.",
                            mapId ? "PET" : "npc",
                            key, static_cast<unsigned>(group),
                            static_cast<long long>(newVal), retAddr,
                            greetReward ? " == REWARD" : "");
                    }
                }
#endif

                auto itBase = g_lastVal.find(ckBase);
                if (itBase == g_lastVal.end() && !greetReward)
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
                    g_lastVal[ckLive] = newVal;
                    return;
                }
                // A greet with no baseline started from nothing; otherwise the
                // group's persisted value is where this relationship stood.
                oldVal = (itBase == g_lastVal.end()) ? 0 : itBase->second;
            }

            const State& st = State::Get();
            const float mult = st.trustMultVal;
            const bool  on   = st.trustMult && mult > 1.0f;


            if (on && newVal > oldVal && oldVal < kFriendly_Max &&
                g_rejected.find(ckLive) == g_rejected.end())
            {
                const int64_t s = ScaleGain(oldVal, newVal, mult);
                if (s != newVal && Write64(r + kOff_FriendlyRec_Value, s))
                {
                    ++g_scaled;
                    g_ourValue.insert(ckLive);
                    // Record what we actually wrote. Skipping this leaves
                    // oldVal frozen at the first value we ever saw, so the
                    // next record for this relationship scales from that
                    // stale baseline again instead of from the new one.
                    g_lastVal[ckLive] = s;
                    return;
                }
            }

            // Falling through with a value BELOW what we last allowed means
            // something handed the true number back - a reload, or the
            // server-authority copy resyncing over our client-side write. We
            // cannot tell those apart here (unlike inventory and equipment,
            // friendly.cpp has no per-realm walk), and reseeding is the right
            // move for a reload, so the value is let through either way. It is
            // counted because a session full of reverts is the signature of the
            // second case, and that is the thing worth seeing in the log.
            //
            // When the value that came back lower is one TRINITY wrote, that is
            // not a reload - the game rejected the write. Give that relationship
            // up rather than re-scale it on the next push, which is the loop
            // described at g_rejected.
            if (newVal < oldVal)
            {
                ++g_reverted;
                if (g_ourValue.erase(ckLive)) g_rejected.insert(ckLive);
            }
            else ++g_passed;
            g_lastVal[ckLive] = newVal;
        }


        // The greet/dialogue award, reached by rewriting one call rather than
        // hooking the callee - see kSig_FriendlyCommitAddCall. Five arguments:
        // the fifth is already homed at the call site, so it has to be
        // forwarded even though nothing here reads it.
        using AddDelta_t = void*(__fastcall*)(void* rel, int32_t* status,
                                              uint16_t group, int64_t delta,
                                              void* extra);
        AddDelta_t oCommitAddDelta = nullptr;
        uintptr_t  g_commitAddCall = 0;

        void* __fastcall hkCommitAddDelta(void* rel, int32_t* status, uint16_t group,
                                          int64_t delta, void* extra)
        {
            const State& st = State::Get();
            if (st.trustMult && st.trustMultVal > 1.0f && delta > 0)
            {
                // No clamp here on purpose. kFriendly_Max is Trinity's own
                // number, not the engine's, and the engine evaluates its tier
                // thresholds inside this call - so handing it the real
                // multiplied delta is what makes tiers advance, rollovers
                // subtract and the tier-up notification fire, instead of
                // Trinity deciding for it.
                const double scaled = static_cast<double>(delta) *
                                      static_cast<double>(st.trustMultVal);
                delta = scaled > 9.0e15 ? static_cast<int64_t>(9.0e15)
                                        : static_cast<int64_t>(scaled);
            }
            return oCommitAddDelta(rel, status, group, delta, extra);
        }

        void* __fastcall hkSetNpc(void* mapOwner, void* record)
        {
            // Taken before anything else: the caller is what says whether this
            // record is a reward or a load.
            const uintptr_t ret = reinterpret_cast<uintptr_t>(_ReturnAddress());
            ScaleRecord(record, 0, ret);
            return oSetNpc(mapOwner, record);
        }

        void* __fastcall hkSetPet(void* mapOwner, void* record)
        {
            ScaleRecord(record, 1, reinterpret_cast<uintptr_t>(_ReturnAddress()));
            return oSetPet(mapOwner, record);
        }

        void* __fastcall hkTrustAdd(void* rel, uint32_t* status,
                                    uint16_t group, int64_t delta)
        {
            const State& st = State::Get();
            // The engine early-outs on delta <= 0 without writing; leave those.
            //
            // Never scale the mount. Riding reaches this same accumulator and a
            // horse's bond does not survive being accelerated - see
            // kSig_FriendlyMountGainCall for the three attempts that proved it.
            // g_mountGainRet == 0 means we could not identify that site in this
            // build, and then nothing is scaled here at all: gifts still go
            // through the record setters, and greet/feed quietly stop being
            // multiplied rather than risking the crash.
            const bool siteOk =
                g_mountGainRet != 0 &&
                reinterpret_cast<uintptr_t>(_ReturnAddress()) != g_mountGainRet;

            if (siteOk && delta > 0 && st.trustMult && st.trustMultVal > 1.0f)
            {
                // Cap so the total cannot pass the ceiling; the engine clamps
                // too, but asking it for less is cheaper than relying on that.
                int64_t cur = 0;
                const int64_t room =
                    (rel && Read64(reinterpret_cast<uintptr_t>(rel) + kOff_FriendlyRel_Trust, &cur)
                         && cur >= 0 && cur < kFriendly_Max)
                        ? kFriendly_Max - cur
                        : kFriendly_Max;

                const double  scaled = static_cast<double>(delta) *
                                       static_cast<double>(st.trustMultVal);
                const int64_t want   = (scaled >= static_cast<double>(room))
                                           ? room
                                           : static_cast<int64_t>(scaled + 0.5);
                if (want > delta)
                {
                    std::lock_guard<std::mutex> lk(g_cacheMx);
                    ++g_gains;
                    g_lastRec = GetTickCount64();
                    delta = want;
                }
            }
            return oTrustAdd(rel, status, group, delta);
        }

    }

    bool Friendly::Install()
    {
        // Both setters are matched inside their bodies, on the map offsets
        // that distinguish them, and located through the unwind tables - see
        // kSig_FriendlySetNpc for why the old prologue patterns are gone.
        const bool npc = mem::InstallHookInterior("friendly: NPC trust setter",
                                                  kSig_FriendlySetNpc,
                                                  "NPC gift Trust Multiplier disabled",
                                                  &hkSetNpc, &oSetNpc, &g_npcTarget);
        const bool pet = mem::InstallHookInterior("friendly: pet trust setter",
                                                  kSig_FriendlySetPet,
                                                  "pet Trust Multiplier disabled",
                                                  &hkSetPet, &oSetPet, &g_petTarget);
        // The delta accumulator is deliberately NOT hooked.
        //
        // It is the path that makes greet and feed scale (0.18.1's answer to
        // "Trust Multiplier is not working"), and it is also the path that has
        // four call sites - one of them the MOUNT. Users report "crash every
        // time on horseback"; reproduced here, and every attempt to make the
        // scaling safe failed: inflating the delta, clamping the resulting
        // total, repeating the call N times, and finally excluding the mount
        // call site outright by return address. That last build multiplied
        // nothing at all and still died at CrimsonDesert.exe+0x239BEF9, right
        // after this accumulator returns.
        //
        // The vTweak fork (Lian/ReXooGen, TU 2.00.02) reaches the same
        // conclusion by construction: its binary carries no
        // kSig_FriendlyAddDelta at all, only the two record setters, and it
        // scopes the feature as "Gifting NPCs or feeding animals".
        //
        // So gifts scale and greet/feed do not. That is a smaller feature than
        // 0.18.1 promised, and it is the one that does not take the game down.
        // Flip this to true to put the hook back for investigation.
        constexpr bool kHookTrustDelta = false;
        const bool add = kHookTrustDelta
            ? mem::InstallHook("friendly: trust delta accumulator",
                               kSig_FriendlyAddDelta,
                               "greet/feed Trust Multiplier disabled",
                               &hkTrustAdd, &oTrustAdd, &g_addTarget)
            : false;
        // InstallHook is silent on success everywhere else, which left a
        // silent log genuinely ambiguous: hook missing, or hook present and
        // never called? These two say which, and cost one line each.
        if (add) LOG("friendly: trust gain accumulator hooked - greet and feed scale.");

        // Teach the setter hooks which callers are rewards. No new hook, no
        // code patching, nothing per-frame: this only changes how the record
        // the existing hooks already receive is interpreted.
        //
        // ONE source, the interaction dispatcher, and deliberately not the
        // 0x14287B8E5 site Trinity shipped first.
        //
        // That site was registered as "the GREET reward" on a static reading of
        // the caller list, and a live log refuted it: a warp produced 37
        // first-sight records through it inside a single second, carrying 5, 10,
        // 15 and 100. No interaction awards 37 NPCs at once, and the values are
        // stored trust rather than awards - it is the streaming push. Treating
        // it as a reward multiplied the save file on every warp, which is the
        // exact failure the seed branch exists to prevent.
        //
        // The dispatcher is different in kind, not just in address: it walks an
        // array of PENDING REWARDS, so every record reaching these two calls is
        // an award by construction. That is a structural guarantee rather than
        // an inference about what a caller probably is, which is what the first
        // attempt got wrong.
        {
            const uintptr_t apply = mem::FindPattern(kSig_FriendlyRewardApply);
            if (apply && mem::CountMatches(kSig_FriendlyRewardApply, 4) == 1)
            {
                g_rewardRets[0] = apply + kOff_RewardApply_NpcRet;
                g_rewardRets[1] = apply + kOff_RewardApply_PetRet;
                LOG("friendly: reward call sites @ %llX %llX - greeting scales.",
                    (unsigned long long)g_rewardRets[0],
                    (unsigned long long)g_rewardRets[1]);
            }
            else
                LOG_WARN("friendly: reward call site %s - gifting and feeding "
                         "still scale, greeting does not.",
                         apply ? "is ambiguous" : "NOT FOUND");
        }

        // Greet and dialogue, at last - by rewriting one call rather than
        // hooking the callee. See kSig_FriendlyCommitAddCall for why the
        // callee itself is off limits. Non-fatal: without it gifts still
        // scale, which is where this feature has been for three releases.
        // DISABLED after a crash report. Do not re-enable without reading this.
        //
        // PatchCall rewrites four bytes INSIDE a live instruction while the
        // game is running. That write is not atomic: if another thread is
        // executing this exact call at that moment it can fetch a half-updated
        // rel32 and jump somewhere arbitrary. Install() runs during startup,
        // when the game is already multi-threaded, so the window is real -
        // and the interaction commit is not obviously cold at that point.
        //
        // Hooking a function via MinHook does not have this problem: it
        // patches an instruction boundary at a function entry and handles the
        // serialisation. Rewriting the middle of somebody else's basic block
        // is a different and worse thing, and I shipped it without saying so.
        //
        // Making it safe needs, at minimum: suspend every other thread, verify
        // none has its RIP inside the five bytes, write, resume - and even
        // then an 8-byte-aligned single write would be the honest way to do
        // it. Until that exists, greet/dialogue does not scale.
        constexpr bool kPatchGreetAwardCall = false;
        const uintptr_t addSite = kPatchGreetAwardCall
                                  ? mem::FindPattern(kSig_FriendlyCommitAddCall) : 0;
        if (addSite && mem::CountMatches(kSig_FriendlyCommitAddCall, 4) == 1)
        {
            g_commitAddCall = addSite + kOff_CommitAddCall_Call;
            if (mem::PatchCall(g_commitAddCall, &hkCommitAddDelta,
                               reinterpret_cast<void**>(&oCommitAddDelta)))
                LOG("friendly: greet/dialogue award call redirected @ %llX - greeting "
                    "and dialogue now scale too.", g_commitAddCall);
            else
            {
                g_commitAddCall = 0;
                LOG_WARN("friendly: greet/dialogue award call could not be redirected - "
                         "only gifting and feeding scale.");
            }
        }
        else if (kPatchGreetAwardCall)
        {
            LOG_WARN("friendly: greet/dialogue award call site %s - only gifting and "
                     "feeding scale.", addSite ? "is ambiguous" : "NOT FOUND");
        }

        // The interaction hook is GONE, and the negative result is the point.
        //
        // It was tried on both halves of the pair - validate (0x1426E9AE0) and
        // commit (0x1426E9C10, which genuinely does load [rec+0x20] at
        // 0x1426E9CD6 and hand it to 0x142ACBFD0) - with and without restoring
        // the original afterwards. Greet trust stayed at the base award every
        // time. The record it scales is not where the greet award comes from;
        // that arrives through the delta accumulator, which stays unhooked
        // because hooking it kills the game on horseback (see kHookTrustDelta).
        //
        // So it detoured a function on the interaction path, changed nothing,
        // and logged "N trust gain(s) multiplied" while doing it - which reads
        // as success to anyone watching the log, and cost several rounds of
        // testing to disprove. A hook that cannot affect the outcome should not
        // be installed, and must never narrate one.
        //
        // kSig_FriendlyNpcCommit and its anchor stay in offsets.h: the research
        // is correct and worth keeping, only the conclusion changed.

        // Resolve the mount call site so hkTrustAdd can refuse to scale it.
        // Failing to find it is not fatal, but it does switch the delta path
        // off - see the comment in hkTrustAdd for why that is the safe side.
        if (add)
        {
            const uintptr_t site = mem::FindPattern(kSig_FriendlyMountGainCall);
            if (site)
            {
                g_mountGainRet = site + kOff_MountGainCall_Ret;
                LOG("friendly: mount gain call site @ %llX - excluded from scaling.",
                    (unsigned long long)(site + kOff_MountGainCall_Ret - 5));
            }
            else
            {
                LOG_WARN("friendly: mount gain call site not found - greet/feed "
                         "scaling disabled this session (gifts unaffected).");
            }
        }

        return npc || pet || add;
    }

    void Friendly::Remove()
    {
        mem::RemoveHook(&g_npcTarget);
        mem::RemoveHook(&g_petTarget);
        mem::RemoveHook(&g_addTarget);
        if (g_commitAddCall && oCommitAddDelta)
        {
            mem::UnpatchCall(g_commitAddCall, reinterpret_cast<void*>(oCommitAddDelta));
            g_commitAddCall = 0;
            oCommitAddDelta = nullptr;
        }
        std::lock_guard<std::mutex> lk(g_cacheMx);
        g_lastVal.clear();
        g_ourValue.clear();
        g_rejected.clear();
g_seeded = g_scaled = g_reverted = g_passed = g_gains = 0;
        g_lastRec = 0;
    }

    void Friendly::Tick()
    {
        // Cheap enough for a per-frame call: one unlocked read of the burst
        // clock, and the lock is only taken once a burst has actually ended.
        const uint64_t last = g_lastRec;
        if (!last || GetTickCount64() - last < kFriendlyBurst_QuietMs) return;

        int    seeded = 0, scaled = 0, reverted = 0, passed = 0, gains = 0;
        size_t givenUp = 0;
        {
            std::lock_guard<std::mutex> lk(g_cacheMx);
            if (!g_lastRec || GetTickCount64() - g_lastRec < kFriendlyBurst_QuietMs) return;
            seeded   = g_seeded;
            scaled   = g_scaled;
            reverted = g_reverted;
            passed   = g_passed;
            gains    = g_gains;
            givenUp  = g_rejected.size();
            g_seeded = g_scaled = g_reverted = g_passed = g_gains = 0;
            g_lastRec = 0;
        }
        // Only speak when the feature DID something. Records arrive in bursts
        // constantly - walking past NPCs is enough - so reporting every burst
        // meant a line every few seconds saying nothing happened, which is how
        // a log stops being read. Seeded and unchanged counts are the normal
        // resting state, not news; they ride along on a line that had a reason
        // to exist anyway.
        if (!gains && !scaled && !reverted) return;
        // The revert COUNT, not just that some happened. A steady stream of
        // reverts is a write loop - Trinity scaling, the game putting its own
        // number back, Trinity scaling the same record again - and that reads
        // identically to healthy operation when the number is hidden behind
        // "some".
        if (reverted)
            LOG("friendly: %d trust gain(s) multiplied, %d reverted by the game "
                "(%zu relationship(s) given up).",
                gains + scaled, reverted, givenUp);
        else
            LOG("friendly: %d trust gain(s) multiplied.", gains + scaled);
    }

    bool Friendly::Ready()
    {
return g_npcTarget != nullptr || g_petTarget != nullptr ||
               g_addTarget != nullptr;
    }
}
