#include "parry.h"

#include <Windows.h>
#include <cstring>

#include "offsets.h"
#include "../core/logger.h"
#include "../mem/scanner.h"

namespace trinity::game
{
    namespace
    {
        //   vsubss  xmm0, xmm2, xmm0        duration = animEnd - animStart
        //   vmulss  xmm1, xmm0, [k]         duration * k
        //   vsubss  xmm2, xmm2, xmm1        threshold = animEnd - duration*k
        //   vcomiss xmm3, xmm2              windowEnd vs threshold
        //   seta    al                      al = windowEnd > threshold
        //   mov     [rsi], al
        //
        // The verdict alone (`vcomiss/seta/mov`) is already unique, but this
        // patch overwrites EXECUTABLE code, so the pattern deliberately
        // includes the threshold arithmetic ahead of it. That arithmetic is
        // what makes the site a parry verdict rather than some other boolean
        // that happens to be produced by a seta - and forcing an unrelated
        // boolean permanently true is a much worse failure than the feature
        // simply not resolving.
        //
        // 2.01.00 rebuilt this: the old pattern read `vcomiss xmm2, xmm3` and
        // now reads `vcomiss xmm3, xmm2`. The operands are swapped, but the
        // MEANING is unchanged and still favourable-when-true: the surrounding
        // code establishes xmm1/xmm3 as the window's start/end and xmm0/xmm2 as
        // the animation's, and the guard branches above require xmm1 <= xmm3
        // and xmm0 < xmm2. So `al = 1` still says "the window reaches the good
        // phase", which is exactly what Easy Parry wants to assert.
        // Unique match (1) at 0x1407FC514 in 2760.
        constexpr const char* kSig_ParryVerdict =
            "C5 EA 5C C0 C5 FA 59 0D ?? ?? ?? ?? C5 EA 5C D1 "
            "C5 F8 2F DA 0F 97 C0 88 06";

        constexpr uintptr_t kSetaOffset = 20;        // into the match
        constexpr uint8_t   kSeta[3] = { 0x0F, 0x97, 0xC0 };  // seta al
        constexpr uint8_t   kForce[3] = { 0xB0, 0x01, 0x90 }; // mov al,1 ; nop

        uintptr_t g_site = 0;    // address of the seta
        bool      g_on   = false;

        // Write three bytes over executable code, restoring protection either
        // way. Failure leaves the site untouched rather than half-written.
        bool WriteCode(uintptr_t addr, const uint8_t (&bytes)[3])
        {
            DWORD old = 0;
            if (!VirtualProtect(reinterpret_cast<void*>(addr), sizeof(bytes),
                                PAGE_EXECUTE_READWRITE, &old))
                return false;
            std::memcpy(reinterpret_cast<void*>(addr), bytes, sizeof(bytes));
            DWORD ignored = 0;
            VirtualProtect(reinterpret_cast<void*>(addr), sizeof(bytes), old, &ignored);
            FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(addr), sizeof(bytes));
            return true;
        }
    }

    bool Parry::Install()
    {
        const uintptr_t hit = mem::FindPattern(kSig_ParryVerdict);
        if (!hit)
        {
            LOG("parry: verdict site not found - Easy Parry disabled.");
            return false;
        }
        if (mem::CountMatches(kSig_ParryVerdict, 4) != 1)
        {
            // Refuse rather than pick one. This patches executable code, and a
            // second match would mean the pattern no longer identifies the
            // thing it was derived from.
            LOG_WARN("parry: verdict site is ambiguous - Easy Parry disabled rather than "
                     "patch a guess.");
            return false;
        }

        g_site = hit + kSetaOffset;
        // Confirm the exact instruction before ever writing over it.
        if (std::memcmp(reinterpret_cast<void*>(g_site), kSeta, sizeof(kSeta)) != 0)
        {
            LOG_WARN("parry: verdict site does not start with the expected instruction - "
                     "Easy Parry disabled.");
            g_site = 0;
            return false;
        }
        LOG("parry: verdict site @ %p - Easy Parry available.", reinterpret_cast<void*>(g_site));
        return true;
    }

    void Parry::Remove()
    {
        if (g_site && g_on)
            WriteCode(g_site, kSeta);   // never leave the game's code modified
        g_site = 0;
        g_on = false;
    }

    bool Parry::Available() { return g_site != 0; }
    bool Parry::Enabled()   { return g_on; }

    void Parry::SetEnabled(bool on)
    {
        if (!g_site || on == g_on) return;
        if (WriteCode(g_site, on ? kForce : kSeta))
        {
            g_on = on;
            LOG("parry: Easy Parry %s.", on ? "on" : "off");
        }
        else
        {
            LOG_WARN("parry: could not write the verdict site - Easy Parry unchanged.");
        }
    }
}
