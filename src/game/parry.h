#pragma once

namespace trinity::game
{
    // Easy Parry.
    //
    // The engine decides a parry in one place. Having read it:
    //
    //     vsubss  xmm0, xmm2, xmm0      ; duration  = t1 - t0
    //     vmulss  xmm1, xmm0, [k]       ; margin    = duration * k
    //     vsubss  xmm3, xmm2, xmm1      ; threshold = t1 - margin
    //     vcomiss xmm2, xmm3
    //     seta    al                    ; success   = windowEnd > threshold
    //     mov     byte ptr [rsi], al    ; the parry result
    //
    // Everything before that point is the overlap test - whether the incoming
    // attack and your guard occupy the same moment at all. Only the final
    // comparison is the timing MARGIN, and that is the part a player misses by
    // a few frames. Replacing `seta al` with `mov al, 1` drops the margin test
    // and keeps the overlap requirement, so a parry still has to be attempted
    // against a real attack - it just no longer has to be frame-perfect.
    //
    // Deliberately a two-instruction code patch rather than a detour. The
    // reference implementation hooks the whole function, which means matching a
    // signature nobody has verified; the patch touches three bytes at a site
    // that is unique in the image, restores them exactly when switched off, and
    // cannot reach save data at all.
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
