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
        // vcomiss xmm2, xmm3 ; seta al ; mov byte ptr [rsi], al
        // Unique in the image: the comparison, the flag it produces and the
        // store of that flag are all in the pattern, so it identifies the parry
        // verdict itself rather than a shape that happens to recur.
        constexpr const char* kSig_ParryVerdict = "C5 F8 2F D3 0F 97 C0 88 06";

        constexpr uintptr_t kSetaOffset = 4;         // into the match
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
