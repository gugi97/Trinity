#pragma once
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <initializer_list>

#include <MinHook.h>

#include "scanner.h"
#include "../core/logger.h"

namespace trinity::mem
{
    // Redirect ONE `call rel32` to a detour, leaving the callee untouched.
    //
    // For when the function itself cannot be hooked but a single call to it
    // can. The case this exists for: FriendlyAddDelta lives in a packed
    // section with split RUNTIME_FUNCTION entries, so a prologue detour
    // corrupts stack unwinding and the game dies - and it is shared with the
    // mount tick, so even a working hook would fire somewhere it must not.
    // Rewriting one call site reaches exactly the caller we mean, touches
    // five bytes of ordinary .text, and leaves every other caller alone.
    //
    // `callInstr` must point AT the 0xE8 opcode. *original receives the
    // callee so the detour can forward to it. Returns false and changes
    // nothing if the byte there is not a call.
    inline bool PatchCall(uintptr_t callInstr, void* detour, void** original)
    {
        if (!callInstr || !detour || !original) return false;
        auto* p = reinterpret_cast<uint8_t*>(callInstr);
        if (*p != 0xE8) return false;

        const uintptr_t callee = ResolveCall(callInstr);
        const intptr_t  rel    = reinterpret_cast<intptr_t>(detour) -
                                 static_cast<intptr_t>(callInstr + 5);
        // rel32 only reaches +-2GB. A detour in our own module is normally
        // well inside that, but "normally" is not a range check.
        if (rel > INT32_MAX || rel < INT32_MIN) return false;

        DWORD old = 0;
        if (!VirtualProtect(p, 5, PAGE_EXECUTE_READWRITE, &old)) return false;
        *reinterpret_cast<int32_t*>(p + 1) = static_cast<int32_t>(rel);
        VirtualProtect(p, 5, old, &old);
        FlushInstructionCache(GetCurrentProcess(), p, 5);

        *original = reinterpret_cast<void*>(callee);
        return true;
    }

    // Point the call back where it came from. `original` is what PatchCall
    // handed out.
    inline void UnpatchCall(uintptr_t callInstr, void* original)
    {
        if (!callInstr || !original) return;
        auto* p = reinterpret_cast<uint8_t*>(callInstr);
        if (*p != 0xE8) return;
        const intptr_t rel = reinterpret_cast<intptr_t>(original) -
                             static_cast<intptr_t>(callInstr + 5);
        if (rel > INT32_MAX || rel < INT32_MIN) return;
        DWORD old = 0;
        if (!VirtualProtect(p, 5, PAGE_EXECUTE_READWRITE, &old)) return;
        *reinterpret_cast<int32_t*>(p + 1) = static_cast<int32_t>(rel);
        VirtualProtect(p, 5, old, &old);
        FlushInstructionCache(GetCurrentProcess(), p, 5);
    }

    // Finds `sig` (refusing an ambiguous match, up to `maxMatches` probes) and
    // installs a MinHook detour at the resolved address - the
    // find/warn/hook/log sequence every game/*.cpp Install() otherwise
    // repeats by hand. `context` prefixes every log line (e.g.
    // "player: stat-accessor") and `consequence` names what's lost on
    // failure (e.g. "God Mode disabled"), so a call site supplies two
    // feature-specific strings instead of three near-duplicate LOG_* calls.
    //
    // On success, *original is set to the trampoline and *target to the
    // hooked address (truthy - keep it to pass to RemoveHook in Remove());
    // both are left null on failure. Returns whether the hook is installed -
    // callers decide whether a false return is fatal for the whole feature
    // or just one sub-feature.
    template <typename Fn>
    bool InstallHook(const char* context, std::string_view sig, const char* consequence,
                      Fn detour, Fn* original, void** target, size_t maxMatches = 8)
    {
        *target = nullptr;
        const uintptr_t addr = FindPattern(sig);
        if (!addr)
        {
            LOG_ERR("%s signature NOT FOUND - %s.", context, consequence);
            return false;
        }

        const size_t matches = CountMatches(sig, maxMatches);
        if (matches != 1)
        {
            LOG_ERR("%s signature ambiguous (%zu); refusing hook - %s.",
                    context, matches, consequence);
            return false;
        }

        void* t = reinterpret_cast<void*>(addr);
        if (MH_CreateHook(t, reinterpret_cast<void*>(detour), reinterpret_cast<void**>(original)) != MH_OK ||
            MH_EnableHook(t) != MH_OK)
        {
            LOG_ERR("%s: failed to install hook - %s.", context, consequence);
            *original = nullptr;
            return false;
        }

        *target = t;
        return true;
    }

    // InstallHook at an address the caller has already resolved.
    //
    // Some functions cannot be identified by their own bytes at all. The
    // weather preset deserialiser is one: fifteen sibling deserialisers share
    // its exact prologue, and the only thing that told them apart was a baked
    // call displacement - precisely the kind of position-dependent detail a
    // recompile destroys. What identifies it durably is the data it
    // references, its own field-name strings, and that is a search rather
    // than a pattern. This is where such a search hands its answer over.
    template <typename Fn>
    bool InstallHookAt(const char* context, uintptr_t addr, const char* consequence,
                       Fn detour, Fn* original, void** target)
    {
        *target = nullptr;
        if (!addr)
        {
            LOG_ERR("%s NOT FOUND - %s.", context, consequence);
            return false;
        }

        void* t = reinterpret_cast<void*>(addr);
        if (MH_CreateHook(t, reinterpret_cast<void*>(detour), reinterpret_cast<void**>(original)) != MH_OK ||
            MH_EnableHook(t) != MH_OK)
        {
            LOG_ERR("%s: failed to install hook - %s.", context, consequence);
            *original = nullptr;
            return false;
        }

        *target = t;
        return true;
    }

    // InstallHook where `sig` matches somewhere INSIDE the target function
    // rather than at its entry; the entry is recovered from the unwind tables
    // (see mem::FunctionEntry).
    //
    // Use this when a function's prologue is not distinctive but its body is.
    // That is the common case after a compiler-level patch: register saves and
    // stack sizes are the allocator's to change, while the struct offsets a
    // function reads are fixed by the data it operates on. Anchoring on the
    // latter means a signature describes what the function DOES, which is the
    // thing that has to stay true for the hook to be correct at all.
    template <typename Fn>
    bool InstallHookInterior(const char* context, std::string_view sig, const char* consequence,
                             Fn detour, Fn* original, void** target, size_t maxMatches = 8)
    {
        *target = nullptr;
        const uintptr_t interior = FindPattern(sig);
        if (!interior)
        {
            LOG_ERR("%s signature NOT FOUND - %s.", context, consequence);
            return false;
        }

        const size_t matches = CountMatches(sig, maxMatches);
        if (matches != 1)
        {
            LOG_ERR("%s signature ambiguous (%zu); refusing hook - %s.",
                    context, matches, consequence);
            return false;
        }

        const uintptr_t addr = FunctionEntry(interior);
        if (!addr)
        {
            LOG_ERR("%s: no unwind data covers the match at %p, so its function start "
                    "cannot be established; refusing hook - %s.",
                    context, reinterpret_cast<void*>(interior), consequence);
            return false;
        }

        void* t = reinterpret_cast<void*>(addr);
        if (MH_CreateHook(t, reinterpret_cast<void*>(detour), reinterpret_cast<void**>(original)) != MH_OK ||
            MH_EnableHook(t) != MH_OK)
        {
            LOG_ERR("%s: failed to install hook - %s.", context, consequence);
            *original = nullptr;
            return false;
        }

        *target = t;
        return true;
    }

    // InstallHook over a primary + fallback pattern list. Index 0 is the
    // current build's pattern; anything after it is a previous build's, kept so
    // one drifted signature degrades to "matched the older shape" instead of a
    // dead feature. Logs which variant matched, because that is exactly the
    // detail a post-patch bug report needs.
    template <typename Fn>
    bool InstallHookAny(const char* context, std::initializer_list<std::string_view> sigs,
                        const char* consequence, Fn detour, Fn* original, void** target)
    {
        *target = nullptr;
        size_t which = 0;
        const uintptr_t addr = FindPatternAny(sigs, &which);
        if (!addr)
        {
            LOG_ERR("%s signature NOT FOUND (tried %zu variants) - %s.",
                    context, sigs.size(), consequence);
            return false;
        }
        if (which > 0)
            LOG_WARN("%s matched fallback pattern #%zu - this game build moved the "
                     "current one, so please report it.", context, which);

        const size_t matches = CountMatches(*(sigs.begin() + which));
        if (matches != 1)
        {
            LOG_ERR("%s signature ambiguous (%zu); refusing hook - %s.",
                    context, matches, consequence);
            return false;
        }

        void* t = reinterpret_cast<void*>(addr);
        if (MH_CreateHook(t, reinterpret_cast<void*>(detour), reinterpret_cast<void**>(original)) != MH_OK ||
            MH_EnableHook(t) != MH_OK)
        {
            LOG_ERR("%s: failed to install hook - %s.", context, consequence);
            *original = nullptr;
            return false;
        }
        *target = t;
        return true;
    }

    // Disables + removes a hook installed via InstallHook and clears
    // *target, so Remove() can call this unconditionally and stays
    // idempotent (a null target is a no-op).
    inline void RemoveHook(void** target)
    {
        if (!*target) return;
        MH_DisableHook(*target);
        MH_RemoveHook(*target);
        *target = nullptr;
    }
}
