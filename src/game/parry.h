#pragma once

namespace trinity::game
{
    // Easy Parry hooks the native evaluator and forces only the parry result.
    // Evade evaluation and the game's own successful verdict remain untouched.
    class Parry
    {
    public:
        static bool Install();   // locate the site; does not modify anything
        static void Remove();    // restore the original bytes if patched

        static bool Available(); // was the site found?
        static bool Enabled();
        static void SetEnabled(bool on);
    };
}
