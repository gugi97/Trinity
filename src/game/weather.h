#pragma once
#include <cstdint>

namespace trinity::game
{
    // Weather control.
    //
    // How this was found, because it is not the usual shape and the next
    // person (or the next game patch) will need the recipe:
    //
    // The engine ships Korean parse-failure strings that read, in effect,
    // "failed to read <_fieldName> of <TypeName>". The code referencing one
    // sits immediately AFTER the read that failed, so the `lea rdx,[base+OFF]`
    // and `mov r8d, SIZE` next to each message give that field's offset and
    // width. Walking the deserialiser at RVA 012C0360 that way yielded the
    // complete GameGlobalEffectInfo_Weather layout - field names included.
    //
    // The catch: unlike every other table we touch, weather is NOT a `*info`
    // table. There is no "gameglobaleffectinfo" name string in the image, so
    // FindTableGlobal cannot reach it, and the deserialiser's single caller is
    // an asset-loading path - weather presets are LOADED RESOURCES, allocated
    // wherever the loader happens to put them. Scanning for them after the
    // fact is guesswork: 738 functions in the image read four or more of these
    // offsets off some base register, and almost none of them are weather.
    //
    // So instead of hunting for the structs, we let the game hand them to us:
    // hook the deserialiser and record its destination pointer every time one
    // is parsed. Trinity is injected at launch, well before any weather asset
    // loads, so the capture is complete rather than partial.
    //
    // What the override then does is re-stamp a handful of fields across every
    // captured preset each tick. This is the same category of change as the
    // Slot Size / Max Stack overrides: preset data is rebuilt from the paks on
    // every load, so nothing here can reach a save file. Turning the toggle off
    // restores each preset's own captured original bytes.
    //
    //  NOTE: the visible sky is a BLEND of presets driven by time of day and
    //  region, not one preset applied whole. Overriding every preset with the
    //  same values is what makes the blend land on those values regardless of
    //  which presets are being mixed - that is deliberate, and it is why the
    //  control is "all weather everywhere" rather than per-region.
    class Weather
    {
    public:
        // Installs the deserialiser hook. Non-fatal: on failure the feature
        // reports NotReady and the rest of the mod is unaffected.
        static bool Install();

        // Restores every captured preset's original bytes, then removes the
        // hook. Never leaves the game's loaded data modified.
        static void Remove();

        // Per-frame, game-thread apply. Cheap and idempotent; a no-op when the
        // override is off or nothing was captured. Driven by the movement
        // tick, same as World::Tick.
        static void Tick();

        // True once the deserialiser hook is installed.
        static bool Ready();

        // How many weather presets have been captured so far. Zero after a
        // successful Install() just means no weather asset has loaded yet -
        // the count climbs as the world streams in.
        static uint32_t PresetCount();

        // The sky presets, for the menu's dropdown. Exposed as a list rather
        // than hardcoded in the UI so adding one means touching a single table.
        static int                SkyCount();
        static const char* const* SkyNames();
    };
}