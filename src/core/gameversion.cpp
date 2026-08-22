#include "gameversion.h"

#include <Windows.h>
#include <cstdio>
#include <cstring>

#include "logger.h"

#pragma comment(lib, "Version.lib")

namespace trinity
{
    namespace
    {
        // Builds Trinity has actually been checked against, newest first. A
        // revision not in this table is not necessarily broken - most patches
        // move nothing this mod reads - but it is untested, and the log should
        // say so rather than imply a guarantee that was never made.
        struct KnownBuild { uint16_t revision; const char* tu; bool verified; };
        constexpr KnownBuild kKnown[] = {
            { 2474, "1.18.02", true  },  // verified: every signature re-checked
            { 2435, "1.18.0",  true  },
            { 2330, "1.17.00", true  },
        };

        GameVersion g_version;
        bool        g_resolved = false;
        char        g_text[32] = "unknown";

        void Resolve()
        {
            if (g_resolved) return;
            g_resolved = true;

            wchar_t path[MAX_PATH] = {};
            if (!GetModuleFileNameW(nullptr, path, MAX_PATH))
                return;

            DWORD ignored = 0;
            const DWORD size = GetFileVersionInfoSizeW(path, &ignored);
            if (!size) return;

            // The game exe is large but its version block is small; a heap
            // buffer keeps this off the loader thread's stack.
            void* data = ::operator new(size, std::nothrow);
            if (!data) return;

            if (GetFileVersionInfoW(path, 0, size, data))
            {
                VS_FIXEDFILEINFO* ffi = nullptr;
                UINT len = 0;
                if (VerQueryValueW(data, L"\\", reinterpret_cast<LPVOID*>(&ffi), &len) &&
                    ffi && len >= sizeof(VS_FIXEDFILEINFO) && ffi->dwSignature == 0xFEEF04BD)
                {
                    g_version.major    = HIWORD(ffi->dwFileVersionMS);
                    g_version.minor    = LOWORD(ffi->dwFileVersionMS);
                    g_version.build    = HIWORD(ffi->dwFileVersionLS);
                    g_version.revision = LOWORD(ffi->dwFileVersionLS);
                    g_version.known    = true;
                    snprintf(g_text, sizeof(g_text), "%u.%u.%u.%u",
                             g_version.major, g_version.minor,
                             g_version.build, g_version.revision);
                }
            }
            ::operator delete(data);
        }
    }

    const char* GameVersion::text() const { return g_text; }

    const char* GameVersion::titleUpdate() const
    {
        if (!known) return "unknown";
        for (const KnownBuild& k : kKnown)
            if (k.revision == revision) return k.tu;
        return "unrecognised";
    }

    bool GameVersion::isVerified() const
    {
        if (!known) return false;
        for (const KnownBuild& k : kKnown)
            if (k.revision == revision) return k.verified;
        return false;
    }

    const GameVersion& CurrentGameVersion()
    {
        Resolve();
        return g_version;
    }

    void LogGameVersion()
    {
        const GameVersion& v = CurrentGameVersion();
        if (!v.known)
        {
            LOG_WARN("version: could not read the game's version - if something "
                     "misbehaves, mention that in any report.");
            return;
        }
        if (v.isVerified())
            LOG("version: Crimson Desert %s (TU %s) - the build this release was checked against.",
                v.text(), v.titleUpdate());
        else
            LOG_WARN("version: Crimson Desert %s - Trinity has not been checked against this "
                     "build. Anything that resolves will work; anything the patch moved will "
                     "disable itself and say so below. Quote this line in a bug report.",
                     v.text());
    }
}
