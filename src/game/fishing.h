#pragma once

namespace trinity::game
{
    // Auto Catch Fish: every catch settles as a success.
    //
    // One hook, not nine. The reference mod this was derived from installs
    // nine; eight only write its own phase log, and its ninth - the input
    // evaluator at 0x140366CC0 - returns the game's own answer untouched on
    // every path, so it changes nothing either. The catch-settle poll is the
    // only site that decides the outcome.
    class Fishing
    {
    public:
        static bool Install();
        static void Remove();

        static bool Available();
        static bool Enabled();
        static void SetEnabled(bool on);  // gate only; the hook stays installed
    };
}
