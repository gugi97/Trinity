#include "scanner.h"

#include <vector>

#include "../core/logger.h"

namespace trinity::mem
{
    const ModuleRegion& GameModule()
    {
        static const ModuleRegion region = []
        {
            ModuleRegion r{};
            const auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
            if (!base)
                return r;

            const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
            if (dos->e_magic != IMAGE_DOS_SIGNATURE)
                return r;

            const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
            if (nt->Signature != IMAGE_NT_SIGNATURE)
                return r;

            r.base = base;
            r.size = nt->OptionalHeader.SizeOfImage;
            return r;
        }();
        return region;
    }

    namespace
    {
        struct ParsedPattern
        {
            std::vector<uint8_t> bytes; // value at each position (0 where wildcard)
            std::vector<bool>    mask;  // true = must match, false = wildcard
        };

        int HexNibble(char c)
        {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        }

        ParsedPattern Parse(std::string_view p)
        {
            ParsedPattern out;
            for (size_t i = 0; i < p.size();)
            {
                const char c = p[i];
                if (c == ' ' || c == '\t') { ++i; continue; }
                if (c == '?')
                {
                    out.bytes.push_back(0);
                    out.mask.push_back(false);
                    ++i;
                    if (i < p.size() && p[i] == '?') ++i;
                    continue;
                }
                const int hi = HexNibble(c);
                if (hi < 0) { ++i; continue; }
                int lo = hi;
                if (i + 1 < p.size() && HexNibble(p[i + 1]) >= 0) { lo = HexNibble(p[i + 1]); i += 2; }
                else                                              { i += 1; }
                out.bytes.push_back(static_cast<uint8_t>((hi << 4) | lo));
                out.mask.push_back(true);
            }
            return out;
        }

        // Committed and readable - the broad test, used for the fallback pass
        // and for the layout diagnostic.
        //
        // The narrow test is IsExecutableAndReadable below. Every pattern in
        // this project describes INSTRUCTIONS, so a match
        // found in a data section is a false positive by construction - and a
        // costly one, because CountMatches uses the count to decide whether a
        // signature still identifies the function it was derived from. Scanning
        // the whole image (SizeOfImage) meant short patterns picked up
        // coincidental byte runs in data and were then reported as ambiguous,
        // which cost two real features: patterns that are provably unique among
        // the game's code were rejected at load. Restricting the scan to
        // executable pages makes the uniqueness check mean what it says, and
        // makes it faster - the data sections here are far larger than the code.
        //
        // PAGE_EXECUTE alone is still excluded: executable but not readable,
        // so we cannot compare bytes there.
        bool IsReadable(DWORD protect)
        {
            if (protect & PAGE_GUARD) return false;
            if (protect == PAGE_NOACCESS || protect == PAGE_EXECUTE) return false;
            const DWORD readable = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
                                   PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
            return (protect & readable) != 0;
        }

        // Readable AND executable - the default the scan uses.
        //
        // Every pattern in this project describes INSTRUCTIONS, so a match in a
        // data section is a false positive by construction, and a costly one:
        // CountMatches uses the count to decide whether a signature still
        // identifies the function it was derived from. Scanning the whole image
        // meant short patterns collected coincidental byte runs out of data and
        // were then rejected as ambiguous - which is exactly what disabled two
        // map-marker trace points that are provably unique among the code.
        bool IsExecutableAndReadable(DWORD protect)
        {
            if (!IsReadable(protect)) return false;
            const DWORD exec = PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
            return (protect & exec) != 0;
        }

        // Merged, contiguous, committed+readable spans within the module image.
        // Merging adjacent regions lets a pattern straddle a page-protection
        // boundary (e.g. across two .text sub-ranges) without being missed.
        std::vector<std::pair<uintptr_t, uintptr_t>> ReadableSpans(const ModuleRegion& mod,
                                                                   bool execOnly = true)
        {
            std::vector<std::pair<uintptr_t, uintptr_t>> spans;
            const uintptr_t end = mod.base + mod.size;
            uintptr_t addr = mod.base;

            MEMORY_BASIC_INFORMATION mbi{};
            while (addr < end && VirtualQuery(reinterpret_cast<LPCVOID>(addr), &mbi, sizeof(mbi)) == sizeof(mbi))
            {
                const uintptr_t regBase = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
                uintptr_t regEnd = regBase + mbi.RegionSize;
                if (regEnd > end) regEnd = end;

                const bool want = execOnly ? IsExecutableAndReadable(mbi.Protect)
                                           : IsReadable(mbi.Protect);
                if (mbi.State == MEM_COMMIT && want)
                {
                    if (!spans.empty() && spans.back().second == regBase)
                        spans.back().second = regEnd;         // merge contiguous
                    else
                        spans.emplace_back(regBase, regEnd);
                }
                addr = regBase + mbi.RegionSize;              // advance by full region
                if (mbi.RegionSize == 0) break;
            }
            return spans;
        }

        // Scan [begin,end) for the pattern, appending up to maxResults hits.
        void ScanSpan(uintptr_t begin, uintptr_t end, const ParsedPattern& pat,
                      size_t firstFixed, uint8_t anchor, bool haveAnchor,
                      std::vector<uintptr_t>& out, size_t maxResults)
        {
            const size_t patLen = pat.bytes.size();
            if (end < begin || (end - begin) < patLen) return;

            const auto* const start = reinterpret_cast<const uint8_t*>(begin);
            const uint8_t* const last = reinterpret_cast<const uint8_t*>(end - patLen);

            for (const uint8_t* p = start; p <= last; ++p)
            {
                if (haveAnchor && p[firstFixed] != anchor)
                    continue;
                bool hit = true;
                for (size_t i = 0; i < patLen; ++i)
                {
                    if (pat.mask[i] && p[i] != pat.bytes[i]) { hit = false; break; }
                }
                if (hit)
                {
                    out.push_back(reinterpret_cast<uintptr_t>(p));
                    if (out.size() >= maxResults) return;
                }
            }
        }

        std::vector<uintptr_t> Scan(std::string_view pattern, const ModuleRegion& mod,
                                    size_t maxResults, bool allowDataFallback = true)
        {
            std::vector<uintptr_t> results;
            if (!mod) return results;

            const ParsedPattern pat = Parse(pattern);
            const size_t patLen = pat.bytes.size();
            if (patLen == 0) return results;

            size_t firstFixed = 0;
            while (firstFixed < patLen && !pat.mask[firstFixed]) ++firstFixed;
            const bool    haveAnchor = firstFixed < patLen;
            const uint8_t anchor     = haveAnchor ? pat.bytes[firstFixed] : 0;

            for (const auto& [begin, end] : ReadableSpans(mod, /*execOnly=*/true))
            {
                ScanSpan(begin, end, pat, firstFixed, anchor, haveAnchor, results, maxResults);
                if (results.size() >= maxResults) break;
            }
            if (!results.empty() || !allowDataFallback) return results;

            // Nothing among the executable pages. This game ships packed, so a
            // region can legitimately still be PAGE_READWRITE at the moment we
            // scan - unpacked, but not yet reprotected - and an executable-only
            // walk would silently lose every signature inside it. Falling back
            // keeps the previous behaviour reachable; saying so out loud keeps
            // it from becoming an invisible second code path, and this line is
            // the first thing to look for when a pattern that always resolved
            // suddenly does not.
            for (const auto& [begin, end] : ReadableSpans(mod, /*execOnly=*/false))
            {
                ScanSpan(begin, end, pat, firstFixed, anchor, haveAnchor, results, maxResults);
                if (results.size() >= maxResults) break;
            }
            if (!results.empty())
                LOG_WARN("scanner: a pattern matched only OUTSIDE executable pages - that "
                         "region is probably not reprotected yet. The match is kept, but "
                         "treat any uniqueness check on it with suspicion.");
            return results;
        }
    }

    uintptr_t FindPattern(std::string_view pattern, const ModuleRegion& mod)
    {
        const auto hits = Scan(pattern, mod, 1);
        return hits.empty() ? 0 : hits.front();
    }

    uintptr_t FindPatternAny(const std::string_view* patterns, size_t count,
                             const ModuleRegion& mod, size_t* which)
    {
        for (size_t i = 0; i < count; ++i)
        {
            if (const uintptr_t hit = FindPattern(patterns[i], mod))
            {
                if (which) *which = i;
                return hit;
            }
        }
        if (which) *which = count;
        return 0;
    }

    uintptr_t FindPatternIf(std::string_view pattern, const ModuleRegion& mod,
                            bool (*visit)(uintptr_t match, void* ctx), void* ctx)
    {
        if (!mod || !visit) return 0;

        const ParsedPattern pat = Parse(pattern);
        const size_t patLen = pat.bytes.size();
        if (patLen == 0) return 0;

        size_t firstFixed = 0;
        while (firstFixed < patLen && !pat.mask[firstFixed]) ++firstFixed;
        const bool    haveAnchor = firstFixed < patLen;
        const uint8_t anchor     = haveAnchor ? pat.bytes[firstFixed] : 0;

        // Executable pages only, and deliberately without Scan's readable-page
        // fallback: this function exists to answer "does the pattern still
        // identify one function", and counting matches out of data is exactly
        // the thing that made that answer wrong.
        for (const auto& [begin, end] : ReadableSpans(mod, /*execOnly=*/true))
        {
            if (end < begin || (end - begin) < patLen) continue;
            const auto* const start = reinterpret_cast<const uint8_t*>(begin);
            const uint8_t* const last = reinterpret_cast<const uint8_t*>(end - patLen);
            for (const uint8_t* p = start; p <= last; ++p)
            {
                if (haveAnchor && p[firstFixed] != anchor)
                    continue;
                bool hit = true;
                for (size_t i = 0; i < patLen; ++i)
                {
                    if (pat.mask[i] && p[i] != pat.bytes[i]) { hit = false; break; }
                }
                if (hit && visit(reinterpret_cast<uintptr_t>(p), ctx))
                    return reinterpret_cast<uintptr_t>(p);
            }
        }
        return 0;
    }

    size_t CountMatches(std::string_view pattern, const ModuleRegion& mod, size_t maxCount)
    {
        // Same two-pass rule as Scan, and the fallback matters here more than
        // it looks.
        //
        // Refusing the fallback seemed right - counting data coincidences is
        // what made short patterns look ambiguous. But this game is packed, and
        // the higher code regions are not marked executable when Install()
        // runs: two 96-byte patterns that are provably unique in the image came
        // back as ZERO matches and their features were skipped. Zero is a worse
        // answer than one, because a pattern that exists is reported as absent.
        //
        // The two-pass shape keeps both properties. When the executable pass
        // finds anything, only those matches are counted, so data can never
        // pollute a real count. Only when code has nowhere executable to be
        // found does the readable pass run - and that is exactly the packed
        // case this exists for.
        return Scan(pattern, mod, maxCount, /*allowDataFallback=*/true).size();
    }

    void LogModuleLayout()
    {
        const ModuleRegion& mod = GameModule();
        if (!mod)
        {
            LOG_ERR("scanner: game module not resolved.");
            return;
        }

        size_t spanCount = 0, execBytes = 0, readBytes = 0;
        const uintptr_t end = mod.base + mod.size;
        uintptr_t addr = mod.base;
        MEMORY_BASIC_INFORMATION mbi{};
        while (addr < end && VirtualQuery(reinterpret_cast<LPCVOID>(addr), &mbi, sizeof(mbi)) == sizeof(mbi))
        {
            if (mbi.State == MEM_COMMIT && IsReadable(mbi.Protect))
            {
                ++spanCount;
                size_t rs = mbi.RegionSize;
                readBytes += rs;
                if (mbi.Protect & (PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY))
                    execBytes += rs;
            }
            addr = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
            if (mbi.RegionSize == 0) break;
        }
        LOG("scanner: module base=0x%p size=0x%zX readable-regions=%zu readable=0x%zX exec=0x%zX",
            reinterpret_cast<void*>(mod.base), mod.size, spanCount, readBytes, execBytes);
    }
}
