#include "fishing.h"

#include <Windows.h>

#include "../core/logger.h"
#include "../core/state.h"
#include "offsets.h"
#include "../mem/hooks.h"
#include "../mem/safe_memory.h"
#include "../hooks/xinput_hook.h"

namespace trinity::game
{
    namespace
    {
        // The catch tick. Measured: the game calls this about every 31 ms for
        // the whole minigame and it answers "has this catch settled yet", so
        // it is a poll, not the one-shot the reference mod's phase tracking
        // assumed. Nothing else in a 45-second sample outside fishing called
        // it, which is why no session gate is needed here.
        constexpr const char* kSig_TrialSettle =
            "48 8B C4 48 89 58 18 55 56 57 41 54 41 55 41 56 41 57 48 8D A8 88 F7 FF";

        // The catch-in-progress tick. This one runs only while a fish is on the
        // line and being reeled, which is the signal the reel drive needs: the
        // settle poll starts at the cast, so driving from there spun the camera
        // for the whole wait, and the fishing state id spans both phases.
        constexpr const char* kSig_CatchIng =
            "48 89 5C 24 08 48 89 74 24 18 57 48 83 EC 60 48 8B DA 48 8B F1 33 FF "
            "80 7A 21 02 0F 84 ?? ?? ?? ?? 4C 8B 02 48 8B D1 48 8D 4C 24 38 E8 ?? ?? ?? ?? 90 "
            "40 38 7C 24 48 74 1C 48 89 7C 24 20 41 B9 06 00 00 00";

        constexpr uintptr_t kOff_Context_State  = 0x20;
        constexpr uintptr_t kOff_Context_Record = 0x30;

        // Fishing state ids, read out of the live game: 4801 covers the cast,
        // the wait AND the reel (the catch settles there - it is the state that
        // reported outcome 1), and 4796 only flickers past. So the state says
        // nothing about whether a fish is on the line; the catch tick does.
        uint64_t g_seen[12] = {};
        int g_seenCount = 0;
        constexpr uintptr_t kOff_Record_Outcome = 0x5C;

        // The outcome is a code, not a flag: the function returns it and the
        // state machine at 0x141202030 turns it into a transition - 0 keeps
        // fishing, 1 lands the catch, 2 and 3 both lose it. 4 is answered
        // with a flat refusal by both callers, so it is left alone.
        constexpr uint8_t kOutcome_Caught = 1;
        constexpr uint8_t kOutcome_Refused = 4;

        using TrialFn    = uint8_t(__fastcall*)(void*, void*);
        using CatchIngFn = void(__fastcall*)(void*, void*);

        TrialFn    g_orig       = nullptr;
        CatchIngFn g_origCatch  = nullptr;
        void* g_target   = nullptr;
        void* g_tgtCatch = nullptr;
        bool g_writeDead = false;

        bool On() { return State::Get().instantFishing; }

        // One line per distinct combination, so the log says which state the
        // reel actually lives in without a line per tick.
        void DiagOnce(uint32_t state, uint8_t code)
        {
            const uint64_t key = (static_cast<uint64_t>(state) << 16) | code;
            for (int i = 0; i < g_seenCount; ++i)
                if (g_seen[i] == key) return;
            if (g_seenCount >= 12) return;
            g_seen[g_seenCount++] = key;
            LOG("fishing/diag: state=%u code=%u", state, code);
        }

        uint32_t StateId(void* context)
        {
            const uintptr_t c = reinterpret_cast<uintptr_t>(context);
            uintptr_t holder = 0;
            uint32_t id = 0;
            if (c < kMinPointer ||
                !mem::ReadPtr(c + kOff_Context_State, &holder) ||
                holder < kMinPointer || !mem::Read32(holder, &id))
                return 0;
            return id;
        }

        uint8_t __fastcall hkTrialSettle(void* actor, void* context)
        {
            const uint8_t real = g_orig ? g_orig(actor, context) : 0;

            if (On())
                DiagOnce(StateId(context), real);
            if (!On() || g_writeDead || real == kOutcome_Caught ||
                real == kOutcome_Refused)
                return real;

            // The record is absent on roughly half the ticks. Those are not
            // ours to answer - the game has nothing to mark yet.
            const uintptr_t ctx = reinterpret_cast<uintptr_t>(context);
            uintptr_t record = 0;
            if (ctx < kMinPointer ||
                !mem::ReadPtr(ctx + kOff_Context_Record, &record) ||
                record < kMinPointer)
                return real;

            if (!mem::Write8(record + kOff_Record_Outcome, kOutcome_Caught))
            {
                g_writeDead = true;
                LOG_WARN("fishing: the catch record faulted on write - Auto "
                         "Catch Fish will stop forcing the result.");
                return real;
            }
            return kOutcome_Caught;
        }

        void __fastcall hkCatchIng(void* actor, void* context)
        {
            if (g_origCatch) g_origCatch(actor, context);
            // Deadline, not a switch: the drive expires on its own, so a tick
            // that never comes back cannot leave the stick spinning.
            if (On())
                hooks::DriveRightStickUntil(GetTickCount64() + 200);
        }
    }

    bool Fishing::Install()
    {
        const bool a = mem::InstallHook("fishing: catch settle", kSig_TrialSettle,
                                        "Auto Catch Fish disabled", hkTrialSettle,
                                        &g_orig, &g_target);
        const bool b = mem::InstallHook("fishing: catch tick", kSig_CatchIng,
                                        "Auto Catch Fish disabled", hkCatchIng,
                                        &g_origCatch, &g_tgtCatch);
        if (!a || !b)
        {
            Remove();
            return false;
        }
        LOG("fishing: catch settle @ %p, catch tick @ %p - Auto Catch Fish "
            "available.", g_target, g_tgtCatch);
        return true;
    }

    void Fishing::Remove()
    {
        mem::RemoveHook(&g_target);
        mem::RemoveHook(&g_tgtCatch);
        g_orig = nullptr;
        g_origCatch = nullptr;
        g_writeDead = false;
    }

    bool Fishing::Available() { return g_target != nullptr && g_tgtCatch != nullptr; }
    bool Fishing::Enabled()   { return Available() && On(); }

    void Fishing::SetEnabled(bool on)
    {
        if (on) g_seenCount = 0;
        LOG("fishing: Auto Catch Fish %s.", on ? "on" : "off");
    }
}
