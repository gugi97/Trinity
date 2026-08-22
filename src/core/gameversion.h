#pragma once
#include <cstdint>

// Which build of Crimson Desert are we actually running inside?
//
// Every signature and struct offset in offsets.h is tied to a game build, and
// the game has now moved three times in as many weeks (1.17.00, 1.18.0,
// 1.18.02), each time shifting a different handful of them. Until now the mod
// simply assumed the build it was compiled against: on any other one, features
// failed with nothing in the log to say the build was the reason.
//
// Detecting the build does not fix a moved offset by itself. What it does is
// make the failure legible - a bug report can say WHICH build it came from -
// and it gives layout choices somewhere to key off when two builds genuinely
// need different numbers.
namespace trinity
{
    struct GameVersion
    {
        // File version from the executable's VERSIONINFO, e.g. 1.0.0.2474.
        uint16_t major = 0, minor = 0, build = 0, revision = 0;
        bool     known = false;   // did the version resolve at all?

        // "1.0.0.2474", or "unknown" when VERSIONINFO could not be read.
        const char* text() const;

        // The Title Update this revision belongs to ("1.18.02"), or "unrecognised"
        // for a build newer or older than any Trinity has been verified against.
        const char* titleUpdate() const;

        // Was the mod compiled and verified against this exact build?
        bool isVerified() const;
    };

    // Resolved once from the running .exe. Safe to call before any hook.
    const GameVersion& CurrentGameVersion();

    // One startup line naming the build and whether it is the verified one.
    void LogGameVersion();
}
