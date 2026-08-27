#include "weather.h"

#include <Windows.h>
#include <atomic>
#include <cmath>
#include <cstring>

#include "../core/logger.h"
#include "../core/state.h"
#include "../mem/hooks.h"

namespace trinity::game
{
    namespace
    {
        // GameGlobalEffectInfo_Weather::Deserialize(reader, dest).
        // Prologue through the first field read - 40 bytes, unique in the
        // image (24 and 32 bytes both match four sibling deserialisers, which
        // is why the pattern runs this long). rcx = the reader object,
        // rdx = the destination struct: the `mov rdi, rdx` keeps the struct
        // base across the first `call [rax+8]`, and that call passes rdx
        // unchanged with r8d = 4 - which is also how we know the first field
        // sits at +0x00.
        // GameGlobalEffectInfo_Weather::Deserialize(reader, dest) @ RVA 012C0360.
        //
        // NINETY-SIX bytes, and every one of them is needed. The prologue this
        // function opens with is shared by fifteen sibling deserialisers - a
        // 40-byte pattern matches all fifteen, and an earlier build of this file
        // shipped exactly that. It hooked the first match in memory order, which
        // was not weather at all, and then wrote floats into a struct of some
        // other type until the SEH guard caught the fault. Hence the length: the
        // pattern only becomes unique once it runs past the shared prologue and
        // the two wildcarded string references into the call that follows them.
        //
        // rcx = the reader object, rdx = the destination struct. The
        // `mov rdi, rdx` keeps the struct base across the first `call [rax+8]`,
        // and that call passes rdx unchanged with r8d = 4 - which is also how we
        // know the first field sits at +0x00.
        // NOTE: the trailing `call` displacement is deliberately NOT wildcarded.
        // It is what makes this unique - wildcard it and the pattern falls back
        // to matching all fifteen sibling deserialisers that share this
        // prologue. The cost is that a patch which moves either function breaks
        // the match, but that failure is LOUD (signature not found) rather than
        // silent, and Install() now refuses on ambiguity too.
        constexpr const char* kSig_WeatherDeserialize =
            "48 89 5C 24 08 57 48 83 EC 60 48 8B 01 41 B8 04 00 00 00 "
            "48 8B FA 48 8B D9 FF 50 08 84 C0 75 4D 48 8D 0D ? ? ? "
            "? 48 89 4C 24 58 4C 8D 0D ? ? ? ? C7 44 24 50 06 00 "
            "00 00 33 C0 89 44 24 48 B1 01 4C 89 4C 24 40 89 44 24 38 "
            "4C 89 4C 24 30 4C 89 4C 24 28 4C 89 4C 24 20 E8 B0 A2 02 "
            "FF";

        // The fields the override drives, read off the deserialiser. The full
        // 0xB8-byte layout is known (see weather.h); these are the ones that
        // actually change what you see out of the window.
        enum Field
        {
            FRain = 0,      // _precipitation
            FSnowAmount,    // _snowAmount
            FSnowRate,      // _snowRate
            FCloudiness,    // _cloudiness
            FCloudAlpha,    // _cloudAlpha
            FCloudDensity,  // _cloudBaseDensity
            FFog,           // _heightFogDensity
            FWind,          // _windSpeed
            FCount
        };

        constexpr uintptr_t kOff[FCount] = {
            0x00, 0x18, 0x1C, 0x04, 0x74, 0x6C, 0x5C, 0x0C
        };

        constexpr size_t   kWeatherStructSize = 0xB8; // last field is +0xB4 (u8)
        constexpr uint32_t kMaxPresets        = 1024;

        struct Preset
        {
            void*   addr = nullptr;
            uint8_t original[kWeatherStructSize] = {};
        };

        Preset                g_presets[kMaxPresets];
        std::atomic<uint32_t> g_count{ 0 };
        std::atomic<bool>     g_applied{ false };
        std::atomic<bool>     g_dead{ false };

        // The largest value the GAME ITSELF authors for each field, across
        // every preset seen so far. This is the whole reason the feature does
        // not have to guess: nothing here knows whether _heightFogDensity is
        // measured in 0..1 or 0..500, and it does not need to - "thick fog" is
        // expressed as a fraction of the thickest fog the game ships. A number
        // invented in this file would be a guess; this one is evidence.
        std::atomic<float> g_authoredMax[FCount];

        // How one group of fields is driven. `mult` scales what the preset
        // authored (0 wipes the effect out, >1 amplifies it); `floorFrac` is the
        // least it may be, as a fraction of g_authoredMax. Two knobs because a
        // multiplier alone can only ever reduce or amplify weather already
        // present in a preset - it can never put rain into a clear one, which
        // is half of what the feature is for.
        struct Rule { float mult; float floorFrac; };

        struct Sky
        {
            const char* name;
            Rule rain, snow, cloud, fog, wind;
        };

        // mult 1 / floorFrac 0 means "leave this group exactly as authored", so
        // each preset below only states what it actually changes.
        constexpr Rule kKeep{ 1.0f, 0.0f };
        constexpr Rule kNone{ 0.0f, 0.0f };

        constexpr Sky kSkies[] = {
            // name        rain           snow           cloud          fog            wind
            { "Clear",     kNone,         kNone,         kNone,         kNone,         kKeep         },
            { "Cloudy",    kNone,         kNone,         {1.0f,0.60f},  kKeep,         kKeep         },
            { "Rain",      {1.0f,0.70f},  kNone,         {1.0f,0.80f},  kKeep,         {1.0f,0.30f}  },
            { "Storm",     {1.0f,1.00f},  kNone,         {1.0f,1.00f},  kKeep,         {1.0f,1.00f}  },
            { "Snow",      kNone,         {1.0f,0.70f},  {1.0f,0.80f},  kKeep,         {1.0f,0.30f}  },
            { "Blizzard",  kNone,         {1.0f,1.00f},  {1.0f,1.00f},  kKeep,         {1.0f,1.00f}  },
            { "Fog",       kKeep,         kKeep,         {1.0f,0.50f},  {1.0f,1.00f},  kKeep         },
        };
        constexpr int kSkyCount = static_cast<int>(sizeof(kSkies) / sizeof(kSkies[0]));

        using DeserializeFn = bool (*)(void*, void*);
        DeserializeFn g_origDeserialize   = nullptr;
        void*         g_deserializeTarget = nullptr;
        inline float ReadF(const void* base, uintptr_t off)
        {
            float v; std::memcpy(&v, static_cast<const uint8_t*>(base) + off, sizeof(v));
            return v;
        }
        inline void WriteF(void* base, uintptr_t off, float v)
        {
            std::memcpy(static_cast<uint8_t*>(base) + off, &v, sizeof(v));
        }

        std::atomic<bool> g_shapeWarned{ false };

        // Does this memory look like a weather preset? Not a checksum - a shape
        // test. Every field the override drives is a finite, non-negative float
        // (an authored preset has no negative rainfall), and the last field in
        // the struct is a bool. Some other type will trip one of these almost
        // at once, which is all this needs to do: keep a wrong hook from being
        // written to, cheaply, on the loader thread.
        bool LooksLikeWeather(const void* s)
        {
            for (int f = 0; f < FCount; ++f)
            {
                const float v = ReadF(s, kOff[f]);
                if (std::isnan(v) || std::isinf(v) || v < 0.0f) return false;
            }
            uint8_t enableClimateTexture = 0;
            std::memcpy(&enableClimateTexture,
                        static_cast<const uint8_t*>(s) + 0xB4, 1);
            return enableClimateTexture <= 1;
        }
        void NoteAuthoredMaxima(const void* s)
        {
            for (int f = 0; f < FCount; ++f)
            {
                const float v = ReadF(s, kOff[f]);
                // Reject NaN and negatives rather than let one bad preset poison
                // the reference every other preset is measured against.
                if (!(v > 0.0f)) continue;
                float cur = g_authoredMax[f].load(std::memory_order_relaxed);
                while (v > cur &&
                       !g_authoredMax[f].compare_exchange_weak(cur, v, std::memory_order_relaxed))
                { }
            }
        }

        // Runs on whichever thread loads the asset, AFTER the game has filled
        // the struct in - so `original` is the real authored preset rather than
        // uninitialised memory.
        bool __fastcall hkDeserialize(void* reader, void* dest)
        {
            const bool ok = g_origDeserialize ? g_origDeserialize(reader, dest) : false;
            if (!ok || !dest || g_dead.load(std::memory_order_relaxed))
                return ok;

            // Second line of defence, and it exists because the first one failed
            // once: even with a unique pattern, verify the thing we were handed
            // actually LOOKS like weather before writing to it. Every field we
            // drive is a finite, non-negative float, and _enableClimateTexture
            // at +0xB4 is a bool. A struct of some other type fails this almost
            // immediately, and a rejected capture is simply never written to.
            if (!LooksLikeWeather(dest))
            {
                if (!g_shapeWarned.exchange(true, std::memory_order_relaxed))
                    LOG_WARN("weather: the hooked function returned a struct that is not a "
                             "weather preset - refusing to write to it. Weather is inert.");
                return ok;
            }

            NoteAuthoredMaxima(dest);

            // Re-capture rather than duplicate an address we already hold: the
            // loader reuses buffers across zone transitions, and a stale
            // `original` would restore some other preset's values.
            const uint32_t have = g_count.load(std::memory_order_acquire);
            for (uint32_t i = 0; i < have; ++i)
            {
                if (g_presets[i].addr == dest)
                {
                    std::memcpy(g_presets[i].original, dest, kWeatherStructSize);
                    return ok;
                }
            }
            if (have >= kMaxPresets)
                return ok;

            // Fill the slot completely before publishing it, so a concurrent
            // Tick either does not see this entry at all or sees it whole.
            g_presets[have].addr = dest;
            std::memcpy(g_presets[have].original, dest, kWeatherStructSize);
            g_count.store(have + 1, std::memory_order_release);

            if (have == 0)
                LOG("weather: first preset captured @ %p - Weather is live.", dest);
            return ok;
        }

        inline float Drive(float orig, const Rule& r, int field, float intensity)
        {
            const float scaled = orig * r.mult;
            const float floorV = r.floorFrac <= 0.0f
                ? 0.0f
                : g_authoredMax[field].load(std::memory_order_relaxed) * r.floorFrac * intensity;
            return scaled > floorV ? scaled : floorV;
        }

        // One preset's worth of writes. Split out so the SEH frame in
        // SweepGuarded holds no C++ object with a destructor.
        void ApplyOne(void* p, const void* o, const Sky& sky, float in)
        {
            WriteF(p, kOff[FRain],         Drive(ReadF(o, kOff[FRain]),         sky.rain,  FRain,         in));
            WriteF(p, kOff[FSnowAmount],   Drive(ReadF(o, kOff[FSnowAmount]),   sky.snow,  FSnowAmount,   in));
            WriteF(p, kOff[FSnowRate],     Drive(ReadF(o, kOff[FSnowRate]),     sky.snow,  FSnowRate,     in));
            WriteF(p, kOff[FCloudiness],   Drive(ReadF(o, kOff[FCloudiness]),   sky.cloud, FCloudiness,   in));
            WriteF(p, kOff[FCloudAlpha],   Drive(ReadF(o, kOff[FCloudAlpha]),   sky.cloud, FCloudAlpha,   in));
            WriteF(p, kOff[FCloudDensity], Drive(ReadF(o, kOff[FCloudDensity]), sky.cloud, FCloudDensity, in));
            WriteF(p, kOff[FFog],          Drive(ReadF(o, kOff[FFog]),          sky.fog,   FFog,          in));
            WriteF(p, kOff[FWind],         Drive(ReadF(o, kOff[FWind]),         sky.wind,  FWind,         in));
        }

        // Presets are game-owned allocations and a zone transition can free one
        // between the capture and the next tick. Rather than pretend a heap
        // pointer can be validated, guard the whole sweep and retire the feature
        // for the session if it ever faults: a dead Weather toggle is
        // recoverable, a crash inside the movement tick is not.
        bool SweepGuarded(bool restore)
        {
            const State& st = State::Get();
            int idx = st.weatherPreset;
            if (idx < 0 || idx >= kSkyCount) idx = 0;
            const Sky&  sky       = kSkies[idx];
            const float intensity = st.weatherIntensity;
            const uint32_t n = g_count.load(std::memory_order_acquire);

            __try
            {
                for (uint32_t i = 0; i < n; ++i)
                {
                    void* p = g_presets[i].addr;
                    if (!p) continue;
                    if (restore)
                        std::memcpy(p, g_presets[i].original, kWeatherStructSize);
                    else
                        ApplyOne(p, g_presets[i].original, sky, intensity);
                }
                return true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }
    }
    bool Weather::Install()
    {
        for (int f = 0; f < FCount; ++f)
            g_authoredMax[f].store(0.0f, std::memory_order_relaxed);

        // Refuse rather than pick one. InstallHook's default is to warn and hook
        // the first match, which is right for a read-only hook and wrong here:
        // this one hands us a pointer we then WRITE to, so a second match does
        // not mean "close enough", it means the pattern no longer identifies the
        // function it was derived from. That exact mistake shipped once already.
        if (mem::CountMatches(kSig_WeatherDeserialize, 4) != 1)
        {
            LOG_WARN("weather: preset deserialiser is ambiguous - Weather disabled rather "
                     "than hook a guess.");
            return false;
        }

        if (!mem::InstallHook("weather: preset deserialiser", kSig_WeatherDeserialize,
                              "Weather control disabled",
                              &hkDeserialize, &g_origDeserialize, &g_deserializeTarget))
            return false;

        LOG("weather: hooked the preset deserialiser - presets are captured as the world loads.");
        return true;
    }

    void Weather::Remove()
    {
        // Put the game's own data back before unhooking and before the DLL goes
        // away: a preset left overridden would otherwise stay that way for the
        // rest of the session with nothing left able to undo it.
        if (g_applied.load(std::memory_order_relaxed) && !g_dead.load(std::memory_order_relaxed))
            SweepGuarded(true);
        g_applied.store(false, std::memory_order_relaxed);
        mem::RemoveHook(&g_deserializeTarget);
        g_origDeserialize = nullptr;
    }

    bool Weather::Ready() { return g_deserializeTarget != nullptr; }

    uint32_t Weather::PresetCount() { return g_count.load(std::memory_order_acquire); }

    int Weather::SkyCount() { return kSkyCount; }

    const char* const* Weather::SkyNames()
    {
        static const char* names[kSkyCount] = {};
        for (int i = 0; i < kSkyCount; ++i) names[i] = kSkies[i].name;
        return names;
    }

    void Weather::Tick()
    {
        if (!g_deserializeTarget || g_dead.load(std::memory_order_relaxed))
            return;

        State& st = State::Get();
        // Nothing captured yet means the world has not streamed weather in; the
        // toggle stays "on" but there is simply nothing to write to yet.
        const bool want = st.weatherOverride && g_count.load(std::memory_order_acquire) > 0;
        const bool have = g_applied.load(std::memory_order_relaxed);

        if (!want)
        {
            // Restore exactly once on the falling edge rather than every frame:
            // the originals are ours to write back, but doing it continuously
            // would fight anything else the game does to these structs.
            if (have)
            {
                if (!SweepGuarded(true))
                {
                    g_dead.store(true, std::memory_order_relaxed);
                    LOG_WARN("weather: a preset went away while restoring - Weather is "
                             "disabled for this session.");
                }
                else
                {
                    LOG("weather: original preset values restored.");
                }
                g_applied.store(false, std::memory_order_relaxed);
            }
            return;
        }

        // Re-applied every tick while on, not once: the game re-reads and
        // re-blends these structs, and new presets stream in as zones load.
        if (!SweepGuarded(false))
        {
            g_dead.store(true, std::memory_order_relaxed);
            g_applied.store(false, std::memory_order_relaxed);
            LOG_WARN("weather: a preset went away while applying - Weather is disabled "
                     "for this session.");
            return;
        }

        if (!have)
        {
            g_applied.store(true, std::memory_order_relaxed);
            int idx = st.weatherPreset;
            if (idx < 0 || idx >= kSkyCount) idx = 0;
            LOG("weather: '%s' applied across %u preset(s) at %.2f intensity.",
                kSkies[idx].name, g_count.load(std::memory_order_acquire), st.weatherIntensity);
        }
    }
}