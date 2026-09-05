#!/usr/bin/env python3
"""Mirror of Friendly::ScaleGain (src/game/friendly.cpp) with its invariants.

The curve is easy to get subtly wrong - it has to be a true no-op at 1.0x, it
has to stay monotonic, and it must never hand the engine a value past the cap,
because an out-of-range trust value is what took the game down on horseback.
Run this after touching ScaleGain:

    python tools/verify_trust_curve.py

Exit code 0 means every invariant held.
"""

MAX = 100  # kFriendly_Max


def scale_gain(old, new, mult):
    """Port of ScaleGain. Keep in step with the C++."""
    gain = new - old
    if gain <= 0 or mult <= 1.0:
        return new
    gap = MAX - old
    if gap <= 0:
        return MAX
    f = gain / gap
    if f >= 1.0:
        return MAX
    scaled = MAX - gap * ((1.0 - f) ** mult)
    if scaled <= new:
        return new
    if scaled >= MAX:
        return MAX
    return int(scaled + 0.5)


MULTS = [1.0, 1.5, 2.0, 3.0, 5.0, 8.0, 12.0, 18.0, 25.0]  # UI range, menu.cpp
FAILURES = []


def check(name, ok, detail=""):
    print(f"  {'ok  ' if ok else 'FAIL'}  {name}{'  ' + detail if detail else ''}")
    if not ok:
        FAILURES.append(name)


def main():
    print("ScaleGain: gift of +5 from 0 across the slider")
    print("  mult :", "  ".join(f"{m:5.1f}" for m in MULTS))
    print("  value:", "  ".join(f"{scale_gain(0, 5, m):5d}" for m in MULTS))
    print()

    # 1.0x must not disturb the game's own number, anywhere.
    identity = all(
        scale_gain(o, o + g, 1.0) == o + g
        for o in range(0, MAX, 3)
        for g in (1, 2, 5, 11)
        if o + g <= MAX
    )
    check("1.0x is a true no-op", identity)

    # Never below what the game granted, never above the cap.
    bounded = True
    for o in range(0, MAX, 3):
        for g in (1, 2, 5, 11, 40):
            if o + g > MAX:
                continue
            for m in MULTS:
                v = scale_gain(o, o + g, m)
                if v < o + g or v > MAX:
                    bounded = False
    check("stays within [game value, cap]", bounded)

    # A bigger multiplier is never worse.
    monotonic = True
    for o in range(0, MAX, 3):
        for g in (1, 5, 11):
            if o + g > MAX:
                continue
            seq = [scale_gain(o, o + g, m) for m in MULTS]
            if any(b < a for a, b in zip(seq, seq[1:])):
                monotonic = False
    check("monotonic in the multiplier", monotonic)

    # The whole slider has to mean something - the flat formula collapsed to a
    # single instant-max value over most of its range, which is the bug here.
    distinct = len({scale_gain(0, 5, m) for m in MULTS})
    check("slider stays meaningful", distinct == len(MULTS),
          f"{distinct}/{len(MULTS)} distinct outcomes")

    # Losses and already-capped targets are the game's business, not ours.
    check("losses pass through", scale_gain(50, 40, 25.0) == 40)
    check("at the cap passes through", scale_gain(MAX, MAX, 25.0) == MAX)

    print()
    if FAILURES:
        print(f"{len(FAILURES)} invariant(s) broken: {', '.join(FAILURES)}")
        return 1
    print("all invariants hold")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
