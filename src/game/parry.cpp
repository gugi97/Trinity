#include "parry.h"

#include <Windows.h>

#include <cstddef>
#include <cstdint>

#include "../core/logger.h"
#include "offsets.h"
#include "../mem/hooks.h"
#include "../mem/safe_memory.h"
#include "../hooks/xinput_hook.h"

namespace trinity::game
{
    namespace
    {
        constexpr const char* kSig_ParryEvaluator =
            "48 8B C4 41 55 41 56 41 57 48 83 EC 70 C5 78 29 40 A8 "
            "48 89 58 08 4C 8D 2D ?? ?? ?? ?? 48 89 68 10 41 B8 02 00 00 00 "
            "44 39 05 ?? ?? ?? ?? 4C 8B FA 48 89 70 18 4C 8B F1";

        using ParryEvaluatorFn = bool(__fastcall*)(void*, void*, float, bool, bool*);

        ParryEvaluatorFn g_original = nullptr;
        void* g_target = nullptr;
        bool g_on = false;
        ULONGLONG g_lastPulse = 0;
        // Rate, not edge. A rising edge of the window looked right and only
        // parried the first attack: consecutive swings keep the window open, so
        // the edge never came back. An interval re-presses through a whole
        // combo, and still leaves the block held between pulses.
        constexpr ULONGLONG kPulseIntervalMs = 250;

        // The verdict byte the evaluator hands back (its fifth argument) says
        // whether the guard landed inside the late slice of the attack's parry
        // window. Forcing it removes that margin: anywhere in the window
        // counts. It does NOT make a held block parry - measured, the player's
        // own guard state machine reacts to the press edge, and this site is
        // the attacker's window, not the player's input.
        // Keyboard and mouse never reach the pad layer: the game reads them
        // through raw input, which delivers events, not state - while a key is
        // held no event arrives, so nothing can be masked on the way in. The
        // release has to be a real event, which is what SendInput posts.
        //
        // No key is configured and none is assumed. Whatever of these is held
        // when the window opens gets released and pressed again, the same rule
        // the pad path follows. Movement keys are deliberately absent.
        constexpr int kPulseKeys[] = {
            VK_RBUTTON, VK_LBUTTON, VK_MBUTTON, VK_XBUTTON1, VK_XBUTTON2,
            VK_LSHIFT, VK_RSHIFT, VK_LCONTROL, VK_SPACE,
            'Q', 'E', 'R', 'F', 'C', 'V',
        };

        void PushMouse(INPUT* in, int& n, DWORD down, DWORD up)
        {
            in[n].type = INPUT_MOUSE;
            in[n].mi = {};
            in[n].mi.dwFlags = up;
            ++n;
            in[n].type = INPUT_MOUSE;
            in[n].mi = {};
            in[n].mi.dwFlags = down;
            ++n;
        }

        void PushKey(INPUT* in, int& n, int vk)
        {
            const WORD scan = static_cast<WORD>(MapVirtualKeyW(vk, MAPVK_VK_TO_VSC));
            for (int pass = 0; pass < 2; ++pass)
            {
                in[n].type = INPUT_KEYBOARD;
                in[n].ki = {};
                in[n].ki.wVk = static_cast<WORD>(vk);
                in[n].ki.wScan = scan;
                in[n].ki.dwFlags = pass == 0 ? KEYEVENTF_KEYUP : 0;
                ++n;
            }
        }

        void PulseHeldKeys()
        {
            INPUT in[2 * (sizeof(kPulseKeys) / sizeof(kPulseKeys[0]))] = {};
            int n = 0;
            for (const int vk : kPulseKeys)
            {
                if (!(GetAsyncKeyState(vk) & 0x8000))
                    continue;
                switch (vk)
                {
                case VK_RBUTTON:  PushMouse(in, n, MOUSEEVENTF_RIGHTDOWN,  MOUSEEVENTF_RIGHTUP);  break;
                case VK_LBUTTON:  PushMouse(in, n, MOUSEEVENTF_LEFTDOWN,   MOUSEEVENTF_LEFTUP);   break;
                case VK_MBUTTON:  PushMouse(in, n, MOUSEEVENTF_MIDDLEDOWN, MOUSEEVENTF_MIDDLEUP); break;
                case VK_XBUTTON1:
                case VK_XBUTTON2: break;   // XBUTTON needs mouseData; not worth it
                default:          PushKey(in, n, vk); break;
                }
            }
            if (n > 0)
                SendInput(static_cast<UINT>(n), in, sizeof(INPUT));
        }

        bool __fastcall hkParryEvaluator(void* a, void* b, float range,
                                         bool evade, bool* perfect)
        {
            const bool eligible = g_original
                ? g_original(a, b, range, evade, perfect)
                : false;
            if (evade || !g_on)
                return eligible;

            // Rising edge of the attacker's parry window: ask the pad layer to
            // drop whatever is held for one poll. The re-press lands inside the
            // window, and that press edge is the only thing that starts a parry.
            const ULONGLONG now = GetTickCount64();
            if (eligible && now - g_lastPulse >= kPulseIntervalMs)
            {
                hooks::PulseButtonRelease();   // pad
                PulseHeldKeys();               // keyboard and mouse
                g_lastPulse = now;
            }
            if (!eligible || !perfect)
                return eligible;

            mem::Write8(reinterpret_cast<uintptr_t>(perfect), 1);
            return eligible;
        }
    }

    bool Parry::Install()
    {
        if (!mem::InstallHook("parry: evaluator", kSig_ParryEvaluator,
                              "Easy Parry disabled", &hkParryEvaluator,
                              &g_original, &g_target))
            return false;

        LOG("parry: evaluator hook installed @ %p - Easy Parry available.", g_target);
        return true;
    }

    void Parry::Remove()
    {
        mem::RemoveHook(&g_target);
        g_original = nullptr;
        g_on = false;
    }

    bool Parry::Available() { return g_target != nullptr; }
    bool Parry::Enabled()   { return g_on; }

    void Parry::SetEnabled(bool on)
    {
        if (!g_target || on == g_on)
            return;

        g_on = on;
        LOG("parry: Easy Parry %s.", on ? "on" : "off");
    }
}
