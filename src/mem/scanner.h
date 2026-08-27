#pragma once
#include <Windows.h>
#include <cstdint>
#include <string_view>
#include <initializer_list>

namespace trinity::mem
{
    // A contiguous mapped module image (base + SizeOfImage).
    struct ModuleRegion
    {
        uintptr_t base = 0;
        size_t    size = 0;
        explicit operator bool() const { return base != 0 && size != 0; }
    };

    // The main game module (the running .exe). Resolved once and cached.
    // Every signature is expressed relative to this module so the mod keeps
    // working across game patches: we never bake in absolute addresses, we scan
    // for the surrounding byte pattern at load time.
    const ModuleRegion& GameModule();

    // Scan a module for an IDA-style byte pattern, e.g.
    //   "48 8B 05 ?? ?? ?? ?? 48 85 C0"
    // '?' or '??' are single-byte wildcards. Returns the address of the first
    // match, or 0 if not present.
    //
    // This game is packed, so we do not trust PE section flags: the scan walks
    // the module's actually-committed pages via VirtualQuery rather than the
    // section table. It prefers pages that are EXECUTABLE, because every
    // pattern here describes instructions and a match in data is a false
    // positive that corrupts the uniqueness checks. If a pattern is found
    // nowhere executable it retries over all readable pages and logs a warning,
    // so a region that has been unpacked but not yet reprotected still resolves.
    uintptr_t FindPattern(std::string_view pattern, const ModuleRegion& mod);
    inline uintptr_t FindPattern(std::string_view pattern) { return FindPattern(pattern, GameModule()); }

    // Try several patterns in order and return the first that resolves, with
    // `which` receiving the index that matched.
    //
    // A signature describes one build's code. When a patch reshapes a function,
    // the old pattern stops matching even though the function is still there
    // and still callable - and a single-pattern lookup turns that into a dead
    // feature. Carrying the previous build's pattern alongside the current one
    // costs a few microseconds at load and keeps the feature alive on both.
    uintptr_t FindPatternAny(const std::string_view* patterns, size_t count,
                             const ModuleRegion& mod, size_t* which = nullptr);
    inline uintptr_t FindPatternAny(std::initializer_list<std::string_view> patterns,
                                    size_t* which = nullptr)
    {
        return FindPatternAny(patterns.begin(), patterns.size(), GameModule(), which);
    }

    // Count matches (up to `maxCount`) for a pattern - used to confirm a
    // signature is unique in the live build before trusting it.
    size_t CountMatches(std::string_view pattern, const ModuleRegion& mod, size_t maxCount = 8);
    inline size_t CountMatches(std::string_view pattern, size_t maxCount = 8) { return CountMatches(pattern, GameModule(), maxCount); }

    // Streaming scan: call `visit(match, ctx)` for every occurrence of the
    // pattern, in address order, until it returns true. Returns the accepted
    // match, or 0 if no match was accepted. Lets a caller filter a very common
    // byte sequence (e.g. a `lea rdx, [rip+..]` opcode) by a semantic test -
    // such as "does this instruction reference *that* string" - without
    // materialising every hit.
    // Executable pages only, no fallback - the predicate is expected to walk
    // backwards into surrounding code, which only makes sense in code.
    uintptr_t FindPatternIf(std::string_view pattern, const ModuleRegion& mod,
                            bool (*visit)(uintptr_t match, void* ctx), void* ctx);
    inline uintptr_t FindPatternIf(std::string_view pattern,
                                   bool (*visit)(uintptr_t, void*), void* ctx)
    {
        return FindPatternIf(pattern, GameModule(), visit, ctx);
    }

    // Log a one-line summary of the module's committed regions (base, size,
    // number of readable/executable pages). Diagnostic aid.
    void LogModuleLayout();

    // Resolve a 32-bit RIP-relative reference. `dispAddr` points at the 4-byte
    // displacement field; `instrEnd` is the address of the next instruction.
    inline uintptr_t ResolveRip(uintptr_t dispAddr, uintptr_t instrEnd)
    {
        return instrEnd + *reinterpret_cast<const int32_t*>(dispAddr);
    }

    inline uintptr_t ResolveRipAt(uintptr_t instr, int instrLen)
    {
        const uintptr_t disp = instr + instrLen - 4;
        return ResolveRip(disp, instr + instrLen);
    }

    inline uintptr_t ResolveCall(uintptr_t callInstr)
    {
        return callInstr + 5 + *reinterpret_cast<const int32_t*>(callInstr + 1);
    }
}
