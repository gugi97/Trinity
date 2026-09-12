#pragma once
#include <cstddef>
#include <cstdint>

// ---------------------------------------------------------------------------
// Trinity - Crimson Desert game interface registry.
//
// This is the ONLY place the mod encodes knowledge about the game binary.
// Everything here is a *byte signature* or a *struct offset* rather than an
// absolute address, so a game patch that shifts code around does not silently
// break us: at load time we re-scan for the signatures below and log if any
// fail to resolve. IDB addresses in comments are RVAs from the analysis dump
// (imagebase 0x0) and are documentation only - never used at runtime.
//
// Engine: Pearl Abyss custom engine ("pa::" namespace). RTTI is intact, so
// class/reflection names are durable anchors across updates.
//
// The stat model below was derived and validated against THIS game build in
// IDA (every signature here is a confirmed unique match).
// ---------------------------------------------------------------------------

namespace trinity::game
{
    // Any pointer below this is treated as bogus (unmapped / small-int garbage).
    inline constexpr uintptr_t kMinPointer = 0x10000000;

    // --- Actor / character layout ------------------------------------------
    // A gameplay character ("owner" object, vtable 0x50B9A10) classifies itself
    // and reaches its stat/vital chain through these offsets:
    //   owner  [0x48]  -> object-type enum (int; see ObjectType)
    //   owner  [0x68]  -> actor
    //   actor  [0x20]  -> status marker
    //   marker [0x18]  -> root / vital owner (see kOff_Marker_TargetOwner)
    // The object-type enum is the engine's own classification (registered in
    // IDB sub_15F530). NOTE: the +0x48 word is NOT how the local player is
    // found - on the live player it flickers (0/3/8) and several characters
    // read 1 at once during combat. The controlled body is resolved by the
    // engine's own predicate instead: player-class type-descriptor tag +
    // possessor round-trip (see kOff_Owner_Possessor / kOff_Owner_TypeDesc).
    // The enum below is kept only as documentation of the classification.
    inline constexpr uintptr_t kOff_Owner_Actor       = 0x68;
    // The actor's navigation component, and the map marker the player placed.
    // Written by the destination request handler 0x142B73330 (which resolves
    // the component the same way: owner+0x68 then actor+0x168) and zeroed by
    // that same handler when the marker is cleared, so all-zero means "no
    // marker" rather than "origin".
    inline constexpr uintptr_t kOff_Actor_NavComp     = 0x168;
    inline constexpr uintptr_t kOff_NavComp_Dest      = 0x1E8; // float[3]
    inline constexpr uintptr_t kOff_Actor_StatusMarker = 0x20; // actor -> status marker
    inline constexpr uintptr_t kOff_Owner_ObjectType  = 0x48;  // int32 ObjectType (documentation only)

    enum ObjectType : int32_t
    {
        Obj_SelfUser            = 0,
        Obj_SelfPlayer          = 1,  // the locally-controlled character
        Obj_GamePlayData        = 2,
        Obj_NonPlayerCharacter  = 3,
        Obj_Mercenary           = 4,
        Obj_Vehicle             = 5,
        Obj_Pet                 = 6,
        Obj_OtherPlayer         = 9,
    };

    // --- Stat / attribute entries ------------------------------------------
    // The engine resolves an attribute to a 0x90-byte "stat entry":
    //   sub_145A5D0(component, attrId) -> entry = [component+0x58] + 0x90*index
    // Layout, reverse-engineered from the commit function sub_C19E1A0 which
    // recomputes and writes these fields:
    //   +0x00 : int32  type id  (see StatType below)
    //   +0x08 : int64  current value  (absolute)   == norm(+0x20) + base(+0x18)
    //   +0x18 : int64  base value
    //   +0x20 : int64  normalized current (current - base)
    //   +0x28 : int64  floor / lower clamp
    //   +0x30 : int64  cap (max) - the value the "is full" flag compares against
    // The commit clamps current DOWNWARD only (a target above current is
    // ignored), so raising a stat requires writing the fields directly.
    inline constexpr uintptr_t kOff_StatEntry_Type    = 0x00; // int32
    inline constexpr uintptr_t kOff_StatEntry_Current = 0x08; // int64 absolute current
    inline constexpr uintptr_t kOff_StatEntry_Base    = 0x18; // int64 base
    inline constexpr uintptr_t kOff_StatEntry_Norm    = 0x20; // int64 current - base
    inline constexpr uintptr_t kOff_StatEntry_Floor   = 0x28; // int64 lower clamp
    inline constexpr uintptr_t kOff_StatEntry_Cap     = 0x30; // int64 max / cap
    // Measured from a live array rather than inferred: entry 0's +0x38 field
    // repeats at +0xC8, putting entry 1 at +0x90, and entry 2 lands at +0x120.
    // An earlier change to 0x98 was wrong - it came from there being no
    // `imul reg, reg, 0x90` in the module, which proves nothing: 0x90 is 9*16
    // and compilers emit it as lea [r+r*8] + shl 4.
    inline constexpr uintptr_t kSizeof_StatEntry      = 0x90; // stride between entries

    // A character's stat entries form ONE contiguous 0x90-stride array with
    // health first (the pointer at root+0x58, i.e. [component+0x58]; entry i =
    // base + 0x90*i). The fresh player resolve reaches this health entry from
    // the SelfPlayer character (kOff_Root_StatArray), then derives the stamina
    // and spirit entries by scanning the array and type-checking each slot.
    // NOTE: a body carries more than one stamina-typed entry - the sprint gauge
    // (type 20) sits several slots past the type-17 meter - so we track ALL
    // stamina-typed slots, not one offset.
    // Type ids run with the array index in this build (entry 1 is type 1,
    // entry 2 is type 2), so the stamina (17), spirit (18), sprint (20) and
    // spirit-pool (21) gauges all sit past the first 16 slots. Scanning only 16
    // found none of them and left both toggles inert. Reads are guarded, so
    // overshooting a shorter array costs nothing.
    inline constexpr int kStatArray_ScanEntries = 32; // slots scanned from health

    // Stat entry type ids (these are the *type* tags stored at entry+0x00, not
    // the attribute enum index). Confirmed against this build.
    enum StatType : int32_t
    {
        StatType_Health   = 0,
        // 1.17.00 moved the whole gauge set up by five (17/18/20/21 ->
        // 22/23/25/26); health stayed 0. Read off the commit funnel while
        // playing: entry 12 committed as type 22 and entry 13 as type 23, and
        // nothing in the old set ever appeared.
        StatType_Stamina  = 22, // a stamina-typed gauge, but NOT the sprint one
        StatType_Spirit   = 23, // internal spirit gauge - NOT the HUD bar
        // The gauge that actually depletes while sprinting. Found empirically:
        // it was the only array slot decreasing during a sprint (base 120000,
        // draining ~7000/s), and unlike the others it carries a real cap.
        // Type 17 pins full without stopping sprint; type 20 is the real gate.
        StatType_SprintSt   = 25,
        // The HUD Spirit bar. Type 18 is a partially-filled internal gauge that
        // never moves on screen; the displayed 0-30 pool (confirmed against the
        // in-game stat screen) is this type instead. Same trap as Stamina above.
        StatType_SpiritPool = 26,
    };
    // NOTE (movement speed): the player stat array also carries two "rate"
    // entries (type 30 and type 74) that rest at 100000 == 1.0x, but writing
    // them has NO effect on locomotion - on-foot movement speed is driven by
    // the character physics move controller, not the stat array. Left for a
    // future milestone; see the project notes.

    // --- Signatures --------------------------------------------------------

    // --- God Mode: guard the single stat-commit choke point ----------------
    // Every HP change - combat, fall damage, drains, heals - is applied by one
    // of several "apply" functions (IDB sub_1459D30 / sub_145B9E0 / sub_145C0F0
    // / sub_145FE10), and ALL of them funnel their final value through ONE
    // commit, pa_StatCommit (IDB sub_BED7820):
    //     int64 pa_StatCommit(void* entry, int64 time, int64 target, uint16 f)
    // It reconstructs current = base(+0x18) + norm(+0x20), clamps it to `target`
    // / floor, then writes current(+0x08) and norm(+0x20). It is the sole writer
    // of the authoritative current-HP field.
    //
    // God Mode hooks it: after the original runs, if the entry is the player's
    // tracked health entry, we force current back to full. Because that happens
    // synchronously inside the commit call - the instant HP is written, before
    // any death check up the stack reads it - a single huge hit (fall damage,
    // one-shots) can never be observed at a lethal value. This is the old
    // per-frame pin moved to the exact write site, which removes the between-
    // frame race that let fall damage kill.
    //
    // pa_StatCommit does not live in the main code blob; it sits in the
    // packer's own region (`.link` in older dumps, `.debug$P` in 2760 - the
    // packer renames its sections every patch). The scanner walks committed
    // executable pages rather than named sections, so that is fine, and it is
    // why nothing here may ever filter by section name.
    //
    // 2760 rewrote it. The old pattern keyed on a prologue that computed
    // base+norm inline (`mov rbx,[rcx+18]; movzx ebp,r9w; add rbx,[rcx+20]`).
    // That arithmetic now lives in a separate CALCULATOR at 0x14171E4C0 which
    // only computes and writes through out-pointers, never touching the entry;
    // pa_StatCommit calls it and remains the sole writer of the entry itself.
    // Do not hook the calculator by mistake - it is the more obvious match for
    // the old comment, it has 16 callers, and hooking it would guard nothing,
    // because the value it returns is still clamped and stored afterwards.
    //
    // Prologue in 2760: mov [rsp+20],r9w; mov [rsp+10],rdx; push rbx/rbp/rsi/
    // rdi/r14; sub rsp,40; lea r14,[rcx+18]; mov rcx,[rcx+20]; add rcx,[r14].
    // The pattern below stops right after `lea r14,[rcx+18]`: the struct
    // offset 0x18 is what gives it meaning, and every byte past it is another
    // instruction-scheduling decision the next patch can reorder. Unique (1).
    //
    // The second argument is no longer a time value - it is a pointer the
    // function stores and later passes on. Trinity forwards all four arguments
    // untouched and only reads the first, so the hook needs no change; the
    // int64_t in StatCommit_t is now carrying a pointer, and that is fine.
    inline constexpr const char* kSig_StatCommit =
        "66 44 89 4C 24 ?? 48 89 54 24 ?? 53 55 56 57 41 56 48 83 EC ?? 4C 8D 71 18";

    // --- Damage multipliers: hook the damage-apply dispatcher ---------------
    // One level above pa_StatCommit sits a per-status "apply signed delta"
    // dispatcher (IDB sub_145B2A0):
    //   int64 pa_StatApplyDelta(void* targetOwner, uint16 statusId, int64 time,
    //                           int64 delta, void* sourceCtx,
    //                           char a6..a10, void* out)
    // It early-outs on delta == 0, routes the primary vital (HP, statusId 0)
    // into a dedicated HP helper (IDB sub_1459400) and every other status into
    // a generic sibling. All battle damage - incoming AND outgoing - passes
    // through it while BOTH sides are still identifiable:
    //   targetOwner          : the victim's vital-owner object; for a tracked
    //                          character it is marker+0x18 (see below)
    //   sourceCtx +0x68      : the attacker's actor. sourceCtx is the same
    //                          object our player chain calls "owner"
    //                          (marker+0x08), so the existing chain offsets
    //                          identify the attacker.
    // Argument/caller layout cross-checked against an earlier game build
    // (sub_1412D8340 there - byte-identical dispatch shape) and re-validated
    // in our dump: unique match at 0x145B2A0.
    inline constexpr const char* kSig_DamageApply =
        "48 89 5C 24 ?? 48 89 6C 24 ?? 48 89 74 24 ?? 57 48 83 EC ?? 49 8B C1 49 8B E8 0F B7 DA 48 8B F1 4D 85 C9";


    // marker+0x18 -> the character's vital/target owner: the object battle
    // damage is addressed to (the `targetOwner` argument above). Validation:
    // its first qword points back at the marker.
    inline constexpr uintptr_t kOff_Marker_TargetOwner = 0x18;

    // --- Fresh player resolution (character manager) -----------------------
    // The churn-proof alternative to the accessor-discovered stat-entry ring:
    // the gameplay-character manager owns a vector of every live character,
    // and the local player is the single one whose ObjectType is SelfPlayer.
    // Resolving it fresh each tick yields an always-current player with no
    // stale cache - a body transition (mount/transform) reallocates the
    // character, but the next resolve simply returns the new one. See the
    // trinity-engine-architecture notes for the full derivation.
    //
    // The manager is read as `manager = *(*G)` from ~30 sites, each of which
    // then passes it as ARG1 to one of the char-manager API functions.
    //
    // Why the original signature broke, and what replaced it. It keyed on the
    // caller's prologue plus the register the manager was cached into:
    //   mov r14,rdx; mov r12,rcx; mov rax,cs:G; mov r15,[rax]; lea rdi,off_...
    // Every one of those registers is the register allocator's free choice. A
    // game update recompiled that function and duly renamed `mov r15,[rax]` ->
    // `mov rsi,[rax]` and `lea rdi` -> `lea r13`, killing the match while the
    // code's MEANING was untouched.
    //
    // The anchors below instead key only on the bytes between the load and the
    // call - `mov rcx,[rax]` (manager is arg1), the arg2/arg3 setup, and
    // literal struct offsets. Those registers are fixed by the Win64 ABI and
    // the constants are fixed by the struct layout, so none of it is the
    // allocator's to rename. Each anchor is independently unique image-wide and
    // resolves the same global; Install() cross-checks that they agree, so a
    // future update has to break all of them at once to disable the feature,
    // and a single stale survivor cannot quietly win the vote.
    //
    // CAUTION: the same API family is also called with two SIBLING globals
    // (0x61830D0 / 0x61830D8 in the current dump) - the client/server realm
    // split, see the trinity-engine-architecture notes. Do NOT loosen these
    // into "any global passed to the char-manager API": that also matches the
    // wrong realm's manager, which resolves fine and then fails silently. Keep
    // every anchor tied to a specific call site.
    //
    // 2.01.00 (build 2760) recompiled every one of these call sites and broke
    // all four at once - not by moving the code, but by rescheduling it. The
    // best anchor of the old set read:
    //
    //     mov rax, cs:G        48 8B 05 <disp>
    //     mov rcx, [rax]       48 8B 08
    //     mov r8,  [r8]
    //     shr r8,  20h
    //
    // and in 2760 the SAME call site reads:
    //
    //     mov r8,  [r8]        4D 8B 00
    //     shr r8,  20h         49 C1 E8 20
    //     lea rdx, [rsp+78h]
    //     mov rcx, cs:G        48 8B 0D <disp>   <- straight into rcx now
    //     mov rcx, [rcx]       48 8B 09
    //     call ...
    //
    // The global is loaded through rcx instead of rax, and the operand setup
    // now precedes the load instead of following it. So the lesson the old
    // comment drew - key on ABI registers and struct literals, not on the
    // allocator's register choices - was right but not sufficient: a pattern
    // that spans several instructions also bets on their ORDER, and that is
    // the compiler's to change. Hence the anchors below are deliberately
    // short. Each covers the operand setup plus the load-and-call idiom and
    // nothing more.
    //
    // Consensus still matters, and 2760 proved why. The old fourth anchor
    // (`mov r8d,[rdi]` + lea + load) survives the patch and now resolves to
    // 0x146C29C68 - a SIBLING realm's manager, exactly the failure the
    // CAUTION above warns about. It is dropped rather than kept as a fallback:
    // an anchor that still matches while pointing somewhere else is worse than
    // one that fails loudly.
    //
    // All four below independently resolve qword_6C29C88 (was qword_61830F8),
    // and each matches exactly once image-wide. Which of the sibling globals
    // is the real character manager is not settled statically - it is settled
    // at runtime by the possessor round-trip in ResolveSelf, which only a
    // genuine character list can satisfy: a wrong manager yields no character
    // at all rather than a wrong one.
    struct CharMgrAnchor
    {
        const char* sig;
        uintptr_t   movOff; // offset of `mov rcx,cs:<global>` (7-byte instr) within the match
    };

    // The SERVER realm's character manager, the sibling of the one above.
    //
    // One init function writes both, eight bytes apart: the server manager
    // into 0x146C29C68 and the client manager into 0x146C29C88. They are the
    // same class, and the code that uses them is cleanly split - in 2760 the
    // server global is loaded from 207 sites across 182 functions clustered
    // in 0x1428A..0x142B6, the client global from 51 sites across 39
    // functions in 0x1426D..0x142A3. Two disjoint bodies of code, one realm
    // each.
    //
    // This is the anchor that was dropped from kCharMgrAnchors earlier today
    // for "resolving to a sibling realm's manager". That was the right
    // observation and the wrong conclusion: it was not a stale anchor
    // pointing somewhere useless, it was pointing at the realm Trinity had
    // no other way to reach.
    //
    // Why it matters: Trinity used to find the server side by watching the
    // transaction commit hook fire during a save load. TU 2.01.00's save
    // loader does not go through that pipeline, so the hook never fired, no
    // candidate was ever captured, and everything gated on the server realm
    // (Add Item, Edit Item, durable dye) stayed locked for the whole
    // session. Walking this global needs no hook and no user action.
    //
    // Verified: exactly one match in 2760, at 0x1428A1C71, resolving
    // 0x146C29C68.
    inline constexpr const char* kSig_CharMgrServer =
        "4D 8B 24 24 48 8D 55 C0 48 8B 0D ?? ?? ?? ?? 48 8B 09 E8";
    inline constexpr uintptr_t kOff_CharMgrServer_Mov = 0x08; // mov rcx,cs:<global>

    inline constexpr CharMgrAnchor kCharMgrAnchors[] = {
        // mov r8,[r8] / shr r8,20h / lea rdx,[rsp+..] / mov rcx,cs:G / mov rcx,[rcx] / call.
        // Best of the set: `shr r8,20h` on an ABI argument is a literal shift
        // count on a fixed register, nothing the allocator can rename. The
        // first three instructions alone are already unique image-wide.
        {"4D 8B 00 49 C1 E8 20 48 8D 54 24 ?? 48 8B 0D ?? ?? ?? ?? 48 8B 09 E8", 12},
        // mov r8d,[rdx+90h] / lea rdx,[rsp+..] / mov rcx,cs:G / mov rcx,[rcx] / call.
        // rdx is the incoming arg2 at entry; 0x90 is a struct offset.
        {"44 8B 82 90 00 00 00 48 8D 54 24 ?? 48 8B 0D ?? ?? ?? ?? 48 8B 09 E8", 12},
        // mov r8d,[rdi] / cmp r8d,[r13+60h] / je / lea rdx,[rsp+..] / mov rcx,cs:G / ...
        // The registers are the allocator's, but 0x60 is a struct offset and
        // the compare-then-branch shape pins the site.
        {"44 8B 07 45 3B 45 60 0F 84 ?? ?? ?? ?? 48 8D 54 24 ?? 48 8B 0D ?? ?? ?? ?? 48 8B 09 E8", 18},
        // mov r8d,[r13] / lea rdx,[rbp+130h] / mov rcx,cs:G / mov rcx,[rcx] / call.
        // Weakest of the set - both the register and the frame offset are the
        // allocator's choice - but it is a fourth independent vote.
        {"45 8B 45 00 48 8D 95 30 01 00 00 48 8B 0D ?? ?? ?? ?? 48 8B 09 E8", 11},
    };

    // Character manager -> the vector of all gameplay characters. It is the
    // engine's custom pa vector (data ptr, then u32 size, u32 capacity);
    // element i is a character* at data + 8*i. Confirmed against the character
    // factory sub_24AA890 (registers each new character via
    // sub_589D00(mgr+0xB8, &char)) and the append helper sub_589D00.
    inline constexpr uintptr_t kOff_CharMgr_ListData  = 0xB8; // character*[] data ptr
    inline constexpr uintptr_t kOff_CharMgr_ListCount = 0xC0; // u32 count
    inline constexpr uint32_t  kCharList_MaxCount     = 8192; // sanity bound (live ~388)

    // Selecting the ONE controlled body among SelfPlayer-typed characters.
    // objType==1 is unique only AT REST (live-confirmed: 388 chars, exactly one
    // type-1). During combat / body transitions the engine spawns transient
    // SelfPlayer-typed characters, so several can carry objType==1 at once and
    // "first type-1 in the vector" flickers between the real body and transients
    // (observed live: the resolved player oscillated every frame).
    //
    // The engine's OWN local-player accessor (IDB sub_2393AA0, the only reader
    // that walks this list to return "the player") disambiguates with a
    // POSSESSOR ROUND-TRIP: the controlled character's possessor/controller at
    // owner+0xA0 points BACK at the character via possessor+0xD0. Only the one
    // body its controller actually possesses satisfies
    //   *(*(owner+0xA0)+0xD0) == owner
    // so this is a deterministic single source of truth, not a heuristic - and
    // it is self-validating: only a real pointer round-trip can match, so a
    // wrong offset resolves to nothing rather than to a wrong character.
    // LIVE-CONFIRMED: exactly one character matches the round-trip per tick,
    // stable across combat/transitions, while the +0x48 objType count swings
    // wildly - the round-trip is the true single source of truth.
    inline constexpr uintptr_t kOff_Owner_Possessor  = 0xA0; // -> possessor/controller
    inline constexpr uintptr_t kOff_Possessor_Pawn   = 0xD0; // -> back-ref to owner

    // The engine's class gate in that accessor is NOT the +0x48 objType word
    // (which on the LIVE player flickers 0/3/8 and is unusable): it reads the
    // type-descriptor at owner+0x88 and tests its tag byte (*(owner+0x88)+1)
    // with ((tag - 1) & 0xF7) == 0, i.e. tag 1 (SelfPlayer) or 9 (OtherPlayer)
    // = a player-class character (see also sub_30DF50, the same tag switch).
    // This tag reads a stable 1 on the player. It is the ONLY type read used.
    inline constexpr uintptr_t kOff_Owner_TypeDesc = 0x88; // -> type descriptor (tag byte at +1)

    // A resolved character IS the god-mode "owner" object (vtable 0x50B9A10):
    // its ObjectType is at +0x48 (kOff_Owner_ObjectType) and its vital chain is
    //   owner -> actor(+0x68) -> marker(+0x20) -> root(+0x18) -> statArray(+0x58)
    // root is the same "component" the stat accessor takes as its argument, so
    // the stat-entry array is the pointer at root+0x58 whose first entry
    // (index 0) is the Health entry. Type-checking that entry as Health
    // validates the whole walk before we trust it.
    inline constexpr uintptr_t kOff_Root_StatArray = 0x58; // ptr -> stat entry[0] (Health)

    // --- World: live position (read-only milestone) ------------------------
    // The per-actor "physics move controller" object carries a 16-byte
    // position vector (x,y,z,w floats) at +0x90. A dedicated SIMD movement-
    // integration function (IDB sub_3A3E140) is called once per tick with
    // that controller as its first (rcx) argument and writes the updated
    // position back to [rcx+0x90] before returning; hooking its ENTRY and
    // reading rcx+0x90 *after* calling through gets the just-updated value
    // with a plain function hook (no mid-instruction/codecave hook needed).
    //
    // NOTE (2026-07-09): statically, this function is reached through a
    // polymorphic per-actor movement-tick dispatch (sub_2F4A720, itself only
    // reachable via a vtable slot), not an obviously input-only/player-
    // exclusive path, so it looked like it might fire for NPCs/mounts too.
    // LIVE-VERIFIED CLEAN, though: the user confirmed the tracked coordinates
    // track the local player correctly in-game (no static player-identity
    // chain to this controller was ever found - the movement-tick dispatch
    // apparently only reaches this path for the player in practice).
    //
    // Prologue: mov rax,rsp; mov [rax+20h],r9; mov [rax+10h],rdx; push rbp;
    // push r14. Unique match in this build.
    inline constexpr const char* kSig_MoveUpdate =
        "48 8B C4 4C 89 48 ? 48 89 50 ? 55 41 56";
    inline constexpr uintptr_t kOff_MoveOwner_Position = 0x90; // x,y,z,w f32
    // The integrator writes the frame's velocity back to +0xD0 (it computes
    // new position = pos + velocity and stores the velocity at [a1+208]).
    // Zeroing this while pinning a teleport keeps the integrator from flinging
    // the proxy on the next tick.
    inline constexpr uintptr_t kOff_MoveOwner_Velocity = 0xD0; // x,y,z,w f32

    // --- Super Jump: scale the integrator's INPUT velocity -------------------
    // Scales the up component of the desired-velocity vector at +0xC0
    // (world-space: [0]=x, [1]=y up, [2]=z) before sub_3A3E140 (the Havok
    // character-proxy integrator) consumes it. Works because a jump is a
    // ballistic impulse: once airborne there is no ground/locomotion authority
    // fighting the scaled velocity. Rising-only (positive y above the
    // threshold) so falling and stair-stepping are never amplified.
    inline constexpr uintptr_t kOff_MoveOwner_DesiredVel = 0xC0; // x,y,z,w f32 (input velocity)
    inline constexpr int       kIdx_MoveOwner_Up         = 1;    // vertical component (y)
    // Only amplify an upward velocity that is clearly a jump/launch, not the
    // small +y jitter of walking over steps/slopes, so ground movement is left
    // alone. Units are the engine's own velocity scale (position is ~cm).
    inline constexpr float     kSuperJump_RiseThreshold  = 1.0f;

    // --- Super Run: scale the locomotion stepper's drive velocity ------------
    // GROUNDED movement speed CANNOT be won at the integrator - three
    // attempts inside sub_3A3E140, all live-disproven (2026-07-14):
    //   (a) flat-multiply the input velocity (+0xC0): pulses on uneven
    //       ground, dead uphill;
    //   (b) scale the resolved position delta (+0x90 diff across the call):
    //       still stuttery;
    //   (c) (a) plus corrections for the integrator's prev-velocity fast-path
    //       heuristic (+0xE0) and a pre-applied ground-constraint clip (its
    //       arg2 plane): downhill perfect, uphill still EXACTLY 1x, rough
    //       roads stutter.
    // Scaling the "CharacterMoveSpeedInfo" data table also had zero effect:
    // each character's actionchart binary (actionchart/bin__/<name>.paac,
    // loader IDB sub_1B47F60) embeds its OWN copy of the move-speed data
    // (AnimationInfo moveSpeedInfoArray) - the global table is design-source
    // only, never read back at runtime.
    //
    // WHY the integrator can't be beaten (HW-watchpoint trace of +0xC0,
    // 2026-07-14): the character movement component runs a SERVO. Per tick it
    //   1. computes a drive velocity,
    //   2. passes it as ARG3 into a sub-step driver (IDB sub_2F49550) that
    //      writes it to moveOwner+0xC0 and calls the integrator itself, then
    //   3. measures the displacement that actually happened and books it back
    //      (IDB sub_2F4DE00 writes +0xC0 = (pos_after - pos_before) / dt).
    // Any velocity injected inside the integrator makes the body overshoot
    // what the servo expected and step 3 pulls it right back - pulsing on the
    // flat, hard 1x clamp uphill.
    //
    // THE FIX (live-verified via Frida arg3 scaling: "worked very well, very
    // smooth", including uphill, up to 10x): hook the sub-step driver and
    // scale the HORIZONTAL components of arg3 before the servo consumes it.
    // Vertical stays untouched (gravity rides in arg3 y at ~-55 while
    // grounded; scaling it would slam the character into the ground).
    //
    // MS x64: a1=rcx component, dt=xmm1 f32, arg3=r8 -> f32 x,y,z drive
    // velocity, a4=r9b, a5..a7 stack. The component holds the move-owner
    // (integrator arg1 / hknp proxy) at +0x298 - not needed for gating (this
    // dispatch only runs for the local player in practice, same as
    // kSig_MoveUpdate above; Super Jump ships ungated on the same evidence).
    //
    // GOTCHA (cost a live debug round-trip): arg3 points at a scratch buffer
    // at a very LOW address (~0x013FDD70) - far below kMinPointer. The
    // mem::Read32/Write32 helpers reject anything under that floor, so reading
    // the drive vector through them silently returns false and the scale
    // no-ops (symptom: hook installs and fires, but every component reads 0.00
    // and speed never changes). Access this vector raw + SEH-guarded instead;
    // the floor is for validating pointer CHAINS, not arguments the callee is
    // about to dereference anyway.
    //
    // Signature = prologue + home-store/push sequence + the exact frame setup
    // (lea rbp,[rax-798h]; sub rsp,860h). The frame displacements are what
    // make it unique - 6 same-shaped functions match if they are wildcarded.
    inline constexpr const char* kSig_LocoStepper =
        "48 8B C4 48 89 58 10 44 88 48 20 48 89 48 08 55 56 57 41 54 "
        "41 55 41 56 41 57 48 8D A8 78 F8 FF FF";
    // The AIRBORNE mover - the caller Free Flight identifies by return
    // address. The loco stepper above is a shared helper: the ground mover and
    // the air mover both call it, and nothing on the component separates a
    // glide from a jump (see the long note at hkLocoStep). So the CALLER is the
    // mode, and Free Flight only acts when the stepper was entered from this
    // function.
    //
    // This used to be a baked module offset (0x307B030 on TU 2.00.00), which is
    // exactly the kind of thing a patch moves: TU 2.00.01 shifted it to
    // 0x307BAB0 and Free Flight went quietly dead - the hook still installed,
    // the range test just never came true again. So it is a signature now like
    // everything else, and teleport.cpp derives the function bounds around the
    // match at load rather than trusting a number from a previous build.
    //
    // The pattern is one of the air mover's own calls INTO the stepper
    // (zeroed r9, a stack out-pointer, the drive vector in xmm1, `this` in rbx).
    // It matches twice, both inside this one function - the stack displacement
    // is wildcarded because the two call sites differ only there - so any match
    // lands in the right place.
    inline constexpr const char* kSig_AirMoverStep =
        "45 33 C9 4C 8D 45 ?? C5 FA 10 0E 48 8B CB E8 ?? ?? ?? ??";

    // --- Fast travel / map-gimmick teleport --------------------------------
    // The world map fast-travels through sub_505140(ignored, sceneId, nodeIndex)
    // (IDB 0x505140): a normal, server-blessed travel that streams properly (the
    // only reliable long-range teleport - every memory-write approach desyncs the
    // Havok body; see trinity-open-questions). The first arg is unused (it feeds
    // sub_5019D0, which pulls the travel manager from a global and ignores it),
    // so we pass nullptr. It validates nodeIndex < nodeCount then triggers travel.
    //   char sub_505140(void* /*ignored*/, int sceneId, unsigned nodeIndex)
    // Prologue: mov rax,rsp; mov [rax+18],rbx; mov [rax+10],edx; mov [rax+8],rcx;
    // push rdi; sub rsp,80h. Unique in this build (IDB 0x505140).
    // 1.18.0 recompiled the prologue: the arguments are now spilled straight to
    // rsp instead of through rax, and it pushes rbp/rsi/rdi, so no amount of
    // wildcarding the old shape matches. Identified by behaviour instead - it
    // is the function that checks sceneId != -1, calls the gimmick scene
    // resolver, and compares desc+0x28 (nodeCount) against its third argument
    // before travelling, which is exactly the documented contract. The frame
    // immediates are wildcarded; the arg shuffle and the sceneId test are not.
    inline constexpr const char* kSig_TravelToNode =
        "48 89 5C 24 ?? 48 89 74 24 ?? 89 54 24 ?? 48 89 4C 24 ?? 55 "
        "57 41 56 48 8D 6C 24 ??";

    // --- Destination map marker update ---------------------------------------
    // kSig_DestinationUpdate is GONE, deliberately. In 2760 it matched
    // 0x140675FE0, which is the road/spawn streamer's terrain ground-clamp:
    // its third argument is the centre of the sector grid being streamed
    // around the player, not a destination. Trinity captured that and warped
    // the player to their own position. The marker is now read straight off
    // the nav component - see kOff_NavComp_Dest - which needs no hook at all.

    // --- Pathing helper ------------------------------------------------------
    // When the movement system is following a map/quest destination, a pathing
    // routine recomputes the desired movement vector every frame and writes it
    // to moveOwner+0x1B0.
    inline constexpr const char* kSig_PathingHelper =
        "48 8B C4 48 89 58 08 48 89 70 18 55 57 41 56 48 8D 68 B1 48 "
        "81 EC ?? ?? ?? ??";

    // Marker origin prefix for coordinate rebasing
    inline constexpr const char* kSig_MarkerOriginPrefix  = "C5 F8 5C 05";
    inline constexpr uintptr_t   kOff_MoveOwner_MarkerVec = 0x1B0;

    // The destinations live in the LevelGimmickSceneObjectInfo registry, a global
    // (IDB qword_6185008), read through its resolver sub_396CC0(u32* sceneId)
    // which returns the scene descriptor (and lazy-loads the data table row on
    // first touch - so it must only be called on the game thread).
    //
    // The resolver body is the shared table-resolver template clone: a byte
    // pattern on it matches ~25 sibling table resolvers (first match in this
    // build is a DIFFERENT table near 0x3318C2 - hooking that produced the
    // v1/v2 garbage menu of four ~25k-node "scenes"). Like the area-name table
    // below, it is found by the string-anchored scan on its unique table-name
    // string instead (see kStr_LevelNameTable for the algorithm).
    //
    // Registry / scene-descriptor / node layout (all live - these tables read
    // zero in the static dump):
    //   registry  +0x08 u32  sceneCount   (resolver keys are 0..count-1)
    //   registry  +0x50 ptr  sceneTable   (desc = *(sceneTable + 8*sceneId),
    //                        null until the resolver lazy-loads the row - use
    //                        the resolver, not the raw slot)
    //   sceneDesc +0x28 u32  nodeCount
    //   sceneDesc +0x20 ptr  nodeArray   (node  = nodeArray + 0xC0*index)
    //   node      +0x10 ptr  gimmick object (a run of std::string members:
    //                        sector keys, an item code, a type template)
    //   node      +0x6c f32  world position x,y,z  (+0x50 is scale, +0x5c a quat)
    // Each "scene" is one gimmick TYPE (scene 0 = bells, 111 = ores, 165 = boards,
    // ...); the node index enumerates every instance of that type on the map.
    inline constexpr const char* kStr_GimmickSceneTable = "LevelGimmickSceneObjectInfo";

    inline constexpr uintptr_t kOff_Registry_SceneCount = 0x08; // u32
    // Same +0x08 shift as kOff_ItemTable_Defs - this registry IS one of those
    // tables, and its resolver walks +0x58 too. This one failed silently: the
    // resolver still resolved, so nothing logged, but every scene descriptor
    // read came off the wrong array.
    inline constexpr uintptr_t kOff_Registry_SceneTable = 0x58; // ptr[]
    inline constexpr uintptr_t kOff_SceneDesc_NodeCount = 0x28; // u32
    inline constexpr uintptr_t kOff_SceneDesc_NodeArray = 0x20; // ptr
    // 1.18.0: 0xC0 -> 0xC8. A known pointer in the node dump drifts exactly +8
    // per node index, which is the stride being 8 short.
    inline constexpr uintptr_t kNode_Stride             = 0xC8;
    // Moved 0x10 -> 0x18: the per-node data pointer (which increments 0x20 per
    // node in the dump) is at +0x18 in this build.
    inline constexpr uintptr_t kOff_Node_Gimmick        = 0x18; // ptr -> gimmick object
    // 1.18.0: 0x6C -> 0x74 (+8, with the node stride). Decoded from the dump:
    // +0x74/+0x78/+0x7C read as a sane world triple (x large, y a plausible
    // height, z), whereas +0x6C gave x=0 and a bogus height.
    inline constexpr uintptr_t kOff_Node_Position       = 0x74; // f32 x,y,z

    // The scene descriptor is the reflected class LevelGimmickSceneObjectInfo
    // (112 bytes; field names recovered from its deserializer's error strings,
    // IDB sub_1175F40). The two fields that fixed the fast-travel menu:
    //   _stringKey  : the scene's authored name ("MineIron_01", "TreasureBox",
    //                 "AbyssRuins_Field", ...) - an engine refcounted string:
    //                 desc+0x08 -> string object -> first qword = char* buffer.
    //   _useTeleport: TRUE only for real fast-travel scenes. This is exactly
    //                 the world map's own filter: sub_B58680 marks a map pin
    //                 travelable only when its scene has this flag (7 scenes in
    //                 the live build: the overworld artifact network, Abyss
    //                 Island/Bridge/Core, housing and the two standstone sets).
    //                 Everything else (ores, chests, bells, shops...) is not a
    //                 travel destination, which is why the unfiltered v1 menu
    //                 "barely worked".
    inline constexpr uintptr_t kOff_SceneDesc_StringKey   = 0x08; // engine string
    inline constexpr uintptr_t kOff_SceneDesc_IsBlocked   = 0x10; // bool
    inline constexpr uintptr_t kOff_SceneDesc_LevelName   = 0x18; // engine string
    inline constexpr uintptr_t kOff_SceneDesc_UseTeleport = 0x49; // bool
    inline constexpr uintptr_t kOff_SceneDesc_IsEmpty     = 0x6C; // bool

    // --- Named area boxes (waypoint display names) --------------------------
    // The game has NO per-node names; its map labels AREAS. The data table
    // "FieldLevelNameTableInfo" (registry global IDB qword_619E708, resolver
    // IDB sub_B7B6850) maps a field id (u32) to a hash map of LevelNameInfo
    // entries, each = a named world-space AABB. Point-in-box over these gives
    // the game's own area name for any position - live-verified: the overworld
    // fast-travel artifacts resolve to "AbyssRuins_Her_0021" (Her = Hernand)
    // style names, region-coded (CD/Del/Dem/Her/Kwe).
    //
    // The resolver's code is a template clone shared by ~24 data-table
    // resolvers, so no byte pattern on it is unique. Resolution is a 3-step,
    // string-anchored scan instead (see Teleport::Install):
    //   1. find the ASCII bytes "FieldLevelNameTableInfo" in the module,
    //   2. find the `lea r8, [rip+..]` (4C 8D 05 ..) that references them
    //      (the lazy-load path passes the table name to the open function),
    //   3. scan BACK from that lea for the resolver's prologue below; the
    //      registry global is the RIP-relative `mov rbx` inside it.
    inline constexpr const char* kStr_LevelNameTable = "FieldLevelNameTableInfo";
    inline constexpr const char* kSig_LeaR8Rip       = "4C 8D 05 ?? ?? ?? ??";
    inline constexpr const char* kSig_TableResolverPrologue =
        "48 89 5C 24 10 48 89 6C 24 18 56 57 41 56 48 83 EC ?? 8B 39 48 8B 1D";
    // Within the prologue: +0x12 is `mov edi,[rcx]`, +0x14 the 7-byte
    // `mov rbx, cs:<registry global>` whose RIP operand we resolve.
    inline constexpr uintptr_t kOff_TableResolver_MovGlobal = 0x14;
    inline constexpr size_t    kLen_MovGlobalInstr          = 7;
    // How far back from the `lea r8` the prologue may sit (it is at +0x5D in
    // this build; give it slack for patch drift).
    inline constexpr size_t    kMax_LeaToPrologue           = 0x180;

    // FieldLevelNameTableInfo row (64 bytes): the LevelNameInfo container is
    // an engine hash map at +0x20..0x3F.
    inline constexpr uintptr_t kOff_LvlRow_BucketCount = 0x20; // u32
    inline constexpr uintptr_t kOff_LvlRow_Size        = 0x24; // u32 (entry count)
    inline constexpr uintptr_t kOff_LvlRow_Buckets     = 0x30; // ptr (0x100-byte buckets)
    inline constexpr uintptr_t kOff_LvlRow_Entries     = 0x38; // ptr (entry* array)
    // Bucket: [count u32 @0, pad, then (hash u32, entryIdx u32) pairs @ +8].
    inline constexpr uintptr_t kLvlBucket_Stride       = 0x100;
    inline constexpr uintptr_t kOff_LvlBucket_Pairs    = 0x08;
    // LevelNameInfo entry (offsets live-verified via hexdump):
    inline constexpr uintptr_t kOff_LvlEntry_Name      = 0x10; // engine string
    inline constexpr uintptr_t kOff_LvlEntry_IsSector  = 0x18; // bool (_isSectorLevel)
    inline constexpr uintptr_t kOff_LvlEntry_Box       = 0x1C; // f32 min xyz, max xyz

    // --- Inventory: item enumeration + quantity editing ---------------------
    // The player's inventory needs no pointer chain from the player object:
    // GetItemQuantity (IDB sub_14A1330) is called by the HUD every time it shows
    // a count, so hooking it captures the live inventory CONTAINER (its 1st arg)
    // within a frame of loading - no transaction, no player walk. From the
    // container, GetInventoryHolder (IDB sub_1CDD520) returns the item holder.
    // Both have unique byte signatures. (Live-confirmed: a walk from here lists
    // every item, and writing a slot's quantity sticks - the game even re-stacks
    // it. Money is the exception: its inventory slot is a passive mirror, so
    // editing it does not change spendable currency.)
    inline constexpr const char* kSig_InvGetItemQty =
        "48 89 5C 24 ?? 66 89 54 24 ?? 55 56 57 48 83 EC 30 48 8B F1 "
        "33 FF 48 8D 4C 24 ??";
    inline constexpr const char* kSig_InvGetHolder =
        "40 53 48 83 EC 20 48 8B 41 ? 48 8B D9 48 8B 48";

    // The engine's OWN slot-expansion setter (IDB sub_1CE8190) - what the game
    // itself runs when your expansion count changes:
    //     void* f(holder, int* outErr, void* unused, u16 bucketType, u16 count)
    // It finds the bucket the same way we do (bucket+0x10 == bucketType), then:
    //     bucket[0x16] = count            ; buff accumulator
    //     bucket[0x1A] = count            ; _varyExpandSlotCount (the real one)
    //     bucket[0x14] = row._defaultSlotCount + bucket[0x1A]
    // Note `count` is the EXPANSION, not the cap: the resulting cap is
    // default + count, so a target cap needs count = cap - _defaultSlotCount.
    // Its final write is NOT clamped to _maxSlotCount (the only _maxSlotCount
    // read gates a dead branch), so counts past the table max do take effect.
    // Preferred over writing kOff_InvBucket_MaxSlots directly, which only
    // pokes a cache the engine recomputes - see kOff_InvBucket_ExpandSlots.
    // 3rd arg is dead (forwarded to a resolver that ignores it): pass nullptr.
    //
    // HOOKED, not just called, because the engine re-stamps VANILLA values
    // through it behind our back (found 2026-07-15 chasing "inventory full"
    // beside a screen of empty slots). The server-side expansion sync (IDB
    // sub_256DD40, fired from event dispatchers e.g. sub_FCC82B0 on event
    // type 83) recomputes the character-inventory expansion from the unlock
    // items the player actually OWNS: it probes six item-type ids from
    // config, picks the highest owned tier, maps it to a count from config
    // globals, drives this setter on the server holder, then replicates the
    // value to the client realm as network message 2137 (sub_243DDF0 packs
    // it; client handler sub_9B7330 -> sub_80ABC0 -> this setter again, after
    // mapping the wire id to a local InventoryType via qword_6181418). So a
    // poked expansion survives only until the next inventory event, in BOTH
    // realms - and a pickup planned inside that window fails the insert
    // planner's cap check while the on-screen grid still shows the raised
    // cap. Substituting the count inside the hook makes the engine's own
    // re-stamps apply the override, which closes the window for good.
    inline constexpr const char* kSig_InvSetExpandSlots =
        "48 89 6C 24 ?? 48 89 74 24 ?? 48 89 7C 24 ?? 41 56 48 83 EC "
        "20 48 8B 41 18 41 0F B7 E9";

    // The FREE-SPACE GATE (IDB sub_1CE8F40) - the check that actually throws
    // "inventory full" on a world pickup, BEFORE the insert planner runs:
    // the server-side give-items transaction (sub_2566C90) calls
    //     free = f(holder, keyPtr, bucketType, &itemTypeId, subType)
    // and refuses with eErrNoInventorySlotNotExist when free <= 0. What it
    // computes (decompiled 2026-07-15):
    //   - bucket = holder bucket with +0x10 == bucketType (0 if none -> full!)
    //   - non-stackable item: free = (i16)cap(+0x14) - (i16)used(+0x12)
    //   - stackable item:     free = (stackMax - owned%stackMax) % stackMax
    //                              + stackMax * max(0, cap - used)
    // Consequences worth remembering: for a stackable item you own a PARTIAL
    // stack of, the first term alone passes the gate even when cap<=used -
    // while an item you own none of fails it. That asymmetry is what
    // "full inventory on SOME pickups" looks like from the outside. Both
    // reads are SIGNED 16-bit, so a cap past 0x7FFF goes negative and fails
    // everything (the menu's 9999 limit keeps us clear of that). This is
    // what confirmed kOff_InvBucket_UsedSlots (below) as the field the mod's
    // quantity editor was leaving stale - see its comment for the fix.

    // Per-holder insert planner (IDB sub_1F850C0). Its 3rd arg (r8) is the
    // inventory CONTAINER; it fires for BOTH the client mirror container AND the
    // server-authority container on every add/reconcile. We hook it purely to
    // CAPTURE the server container (the one whose holder != the client walk
    // holder) - the durable global walk only reaches the client, and there is
    // no client->server pointer link (live-confirmed: midscan + linkscan both
    // empty). Editing a quantity in the client holder alone reverts because a
    // per-frame server reconcile overwrites it; writing the SAME slot in BOTH
    // holders makes the edit real, usable, and non-reverting (live-proven).
    // Unique byte signature.
    inline constexpr const char* kSig_InvHolderInsert =
        "48 89 5C 24 ?? 4C 89 44 24 ?? 48 89 54 24 ?? 48 89 4C 24 ?? "
        "55 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 ?? ?? ?? ?? 48 "
        "81 EC ?? ?? ?? ?? 49 8B D8";

    // Inventory transaction COMMIT (IDB sub_1CE1E70), called by the transaction
    // orchestrator sub_1CC15C0 as `commit(holder, &err, CONTAINER, ...)` - its
    // 3rd arg (r8) is the container. This is the capture point for the
    // SERVER-AUTHORITY container, and the reason is worth spelling out:
    //
    // Loading a save is itself a big inventory transaction, and it drives commit
    // for EVERY realm's container - the server ones FIRST, before the client
    // container object even exists. Live-proven (attach at the title screen,
    // load a save, touch nothing):
    //     0x..0f0200  (server arena)   <- 1st
    //     0x..0f0500  (server arena)   <- 2nd  } both getHolder() to the SAME holder
    //     0x..bf48ac0 (client)         <- 3rd, LAST
    // So the server holder is available seconds after load with no player action.
    // holderInsert (above) does NOT fire at load - it only fires on a real
    // add/drop/buy, which is why edits used to need a "calibration" pickup.
    //
    // Do NOT gate the capture on resolving the client container first: at the
    // moment the server containers go by, that resolve is guaranteed to fail
    // (its global/mid/container chain is not built yet) and the capture is lost.
    // Record every distinct container here, decide which is the server one later.
    //
    // There is NO durable pointer chain to the server container - exhaustively
    // ruled out (2026-07-15): not <=2 hops from either world root
    // (qword_6180C28 / qword_6180C78, which both lead only to the CLIENT actor),
    // no module-global points at it, and the handle-keyed actor registry
    // (sub_2D889E0, world+0x110) does not contain the player at all. It always
    // lands at arena+0xF0200 in a 16MB-aligned server arena, but nothing
    // reachable points at that arena. Capture-at-load is the route; this is it.
    // Unique byte signature.
    inline constexpr const char* kSig_InvCommit =
        "48 89 5C 24 ?? 4C 89 44 24 ?? 48 89 54 24 ?? 48 89 4C 24 ?? "
        "55 56 57 41 54 41 55 41 56 41 57 48 8D 6C 24 ?? 48 81 EC ?? "
        "?? ?? ?? 4D 8B F9 4D 8B E0";

    // Fallback container resolution (the hook above only captures the
    // container when the game happens to query an item count, which is NOT
    // guaranteed at load - live sessions showed it hit-or-miss). The durable
    // path starts at the core global singleton (IDB qword_6180C28):
    //   global -> +0x30 -> +0x50 = inventory container
    //   container -> +0x68 -> +0xB8 = item holder
    // (That holder walk is GetInventoryHolder's own main path - its decompile
    // reads [[container+0x68]+0xB8] after a container-type check; instead of
    // replicating the type check we validate the RESULT structurally, i.e.
    // the holder must expose a sane bucket array. Live-confirmed: this walk
    // resolves the same container the hook captures, from load, with no
    // transaction.)
    // The global is anchored by the travel-manager wrapper (IDB sub_5019D0),
    // whose body is `mov rax, cs:<global>; mov rdx,[rax+30h]; mov rdx,[rdx+50h]`
    // - the exact chain we walk. Unique match; the mov's RIP operand is at
    // match+0x15 (7-byte instruction).
    inline constexpr const char* kSig_InvCoreGlobal =
        "48 8B 0E 48 8B 49 08 E8 ?? ?? ?? ?? 84 C0 0F 85 ?? ?? ?? ?? "
        "48 8B 05 ?? ?? ?? ?? 48 8B 48 30 48 8B 59 50";
    inline constexpr uintptr_t kOff_InvCoreGlobal_Mov = 0x14; // mov rcx, cs:<global>
    inline constexpr uintptr_t kOff_Global_Mid        = 0x30; // global+0x30 -> mid
    inline constexpr uintptr_t kOff_Mid_Container     = 0x50; // mid+0x50 -> container
    inline constexpr uintptr_t kOff_Container_Sub     = 0x68; // container+0x68 -> sub-object
    inline constexpr uintptr_t kOff_Sub_Holder        = 0xB8; // sub+0xB8 = item holder

    // holder -> buckets -> 192-byte item slots.
    //
    // A bucket is not a "type group" - it IS one of the game's storages, and
    // bucket+0x10 says which (see kStr_InventoryInfoTable below). That is why
    // one walk of this holder yields your pack, your Private Storage, your
    // Wardrobe and the Bank all at once, and why a stack in the Bank and one in
    // your pack used to show up as two identical, unexplained rows.
    //
    // Proven by the engine's own bucket lookup (IDB sub_1CE1020), which routes
    // an item to a bucket by matching that field against the item's default
    // storage:
    //     v9 = *(u16*)(itemDef + 66);          // ItemInfo._defaultPushInventoryInfo
    //     for (bucket : holder+0x18 .. +0x20)  // same array we walk
    //         if (*(u16*)(bucket + 0x10) == v9) break;
    // (It can be overridden per item via sub_1CEB790 - which is how the same
    // item can live in the Bank as well as in your pack.)
    inline constexpr uintptr_t kOff_InvHolder_Buckets = 0x18; // ptr[]
    inline constexpr uintptr_t kOff_InvHolder_Count   = 0x20; // u32 bucket count
    inline constexpr uintptr_t kOff_InvBucket_Slots   = 0x00; // ptr[] (slot array)
    // The slot array is a vector: data at +0x00, SIZE at +0x08, capacity at
    // +0x0C. Proven by the push path (IDB sub_ED65670), which grows it on
    // demand - `if (size <= idx) { if (cap < idx) realloc; resize(idx); }`.
    // Size is large headroom (~1460), NOT the storage's slot cap: the cap is
    // kOff_InvBucket_MaxSlots below. It still matters, because the insert
    // planner scans min(cap, size) - see kOff_InvBucket_ExpandSlots.
    inline constexpr uintptr_t kOff_InvBucket_Count   = 0x08; // u16 slot-array SIZE
    inline constexpr uintptr_t kOff_InvBucket_Type    = 0x10; // u16 InventoryType (which storage)
    // The LIVE effective slot cap for this bucket/storage - found 2026-07-15
    // via the RTTI-vtable route (type descriptor ".?AVCommonVaryMaxExpand-
    // InventorySlotBuffProcessor@pa@@" -> COL -> vtable -> the buff-apply
    // slot), not a memory hunt: decompiling that class's apply handler
    // (sub_1C6AB50) shows it resolves the holder (sub_1CDD520, our own
    // kSig_InvGetHolder) then calls a chain that bottoms out in
    // sub_1F85E70(bucket, out, deltaI16) - which finds the bucket the SAME
    // way we do (matches bucket+0x10 == bucketType), clamps against the
    // InventoryInfo row's own _defaultSlotCount/_maxSlotCount (its own
    // +0x48/+0x4A reads - same offsets as kOff_InvDef_DefSlots/MaxSlots
    // above), and writes the result here. This is the number an "is this
    // storage full" check would read - NOT the array capacity at bucket+0x08
    // (that is fixed headroom, ~1460, unrelated to what the UI shows/enforces).
    inline constexpr uintptr_t kOff_InvBucket_MaxSlots = 0x14; // u16, LIVE cap - write target
    // Two accompanying accumulators the same function also updates (raw and
    // clamped running totals of every delta ever applied to this bucket) -
    // not needed for an absolute set, kept here for completeness/future use.
    inline constexpr uintptr_t kOff_InvBucket_DeltaRaw    = 0x16; // u16
    inline constexpr uintptr_t kOff_InvBucket_DeltaClamped = 0x18; // u16

    // How many slots of this storage are IN USE. Both bucket constructors
    // (IDB sub_1CE84A0 / sub_DEB14F0) zero it, and the free-space getter
    // (sub_1CE8F40) is literally `bucket+0x14 - bucket+0x12`; the insert
    // planner's full-check (sub_1F850C0) is
    //     needed + bucket[0x12] > min(bucket[0x14], slot-array size)
    // which is why the array size above still matters.
    //
    // It is an INCREMENTAL accumulator, never recomputed from a scan: the
    // push path does `used += ceil(newQty/stackMax) - ceil(oldQty/stackMax)`.
    // Two consequences worth knowing: picking up an item that stacks onto an
    // existing stack without crossing a slot boundary legitimately moves this
    // by ZERO (not a bug), and writing a slot's quantity directly - as the
    // quantity editor does - bypasses the only code that maintains this, so
    // it goes stale.
    //
    // And the stale case is not cosmetic (LIVE-CAUGHT 2026-07-15): loading a
    // save rebuilds every bucket by pushing each saved stack through that
    // ceil math, so one editor-made stack of 999999 against a vanilla
    // stackMax of 50 books 20000 "used" slots on the spot - a real bucket
    // read used=27445 with cap=2000, at which point the insert planner AND
    // the pickup free-space gate (kSig_InvFreeSpace) refuse everything:
    // "inventory full" beside a screen of empty slots, locked slots in the
    // grouped UI. Inventory::Tick's RepairUsedSlots heals it by clamping
    // this DOWN to physical occupancy (1 per occupied slot - identical to
    // the engine's own accounting in any state the engine produced itself,
    // since it splits stacks at stackMax). Clamp down only, never up, and
    // let the engine keep applying its own deltas on top.
    inline constexpr uintptr_t kOff_InvBucket_UsedSlots = 0x12; // u16, used count
    // The storage's EXPANSION count - the "extra slots you own" beyond the
    // InventoryInfo row's _defaultSlotCount, and the value that actually
    // drives the cap. kOff_InvBucket_MaxSlots is a DERIVED cache of it:
    //     bucket[0x14] = row._defaultSlotCount + bucket[0x1A]
    // recomputed by the engine's own setter (IDB sub_1CE8190, our
    // kSig_InvSetExpandSlots) - and separately as
    //     bucket[0x14] = clamp(row._defaultSlotCount + bucket[0x16],
    //                          row._maxSlotCount)
    // by the slot-expansion buff path (sub_1F85E70). So writing 0x14 alone
    // is poking a cache: any expansion sync or buff apply/expire recomputes
    // it from 0x1A/0x16 and reverts us. Set this instead (or better, call
    // the setter, which maintains 0x16 + 0x1A + 0x14 together).
    //
    // The game's own save/replication schema names it, which is how it was
    // identified - the deserialiser strings spell the record out:
    //     InventoryElementSaveData { _inventoryKey    : InventoryType
    //                                _varyExpandSlotCount : TInventorySlotNo
    //                                _itemList }
    // (Same technique as the ItemInfo/ItemGroupInfo field maps: the table
    // deserialisers name each C++ member.) Nothing else in the binary reads
    // or writes 0x1A - sub_1CE8190 is its sole accessor.
    inline constexpr uintptr_t kOff_InvBucket_ExpandSlots = 0x1A; // u16, _varyExpandSlotCount

    // Quest-reward headroom, applied by RepairUsedSlots. Free space is
    // bucket[0x14] - bucket[0x12], and the insert planner refuses anything it
    // cannot fit; a quest reward has nowhere else to go, so a storage sitting
    // at its cap fails the transaction rather than merely reporting "full".
    // Trigger tight and margin small: this should be invisible on any storage
    // that has room, and is not meant to be a way to expand one.
    inline constexpr int kInvHeadroom_Trigger = 5;    // within this many of cap
    inline constexpr int kInvHeadroom_Slots   = 20;   // free slots to guarantee
    inline constexpr int kInvHeadroom_CapMax  = 4000; // never widen past this

    // Same +8 as kItemVal_Size, and for the same reason - a slot IS a
    // TrItemValue. Confirmed independently in the engine's own slot writer
    // (reached from commit), which walks the live array with
    // `imul rsi, rax, 0xC8`.
    inline constexpr uintptr_t kInvSlot_Stride        = 0xC8; // 200-byte slots
    inline constexpr uintptr_t kOff_InvSlot_TypeId    = 0x08; // u16 item type id
    inline constexpr uintptr_t kOff_InvSlot_Quantity  = 0x10; // i64 quantity (edit here)
    inline constexpr uint16_t  kInvSlot_EmptyType     = 0xFFFF;

    // --- Creating an item from nothing (the add-item path) --------------------
    // A slot IS a TrItemValue (same 0xC0 stride), and the game's own recipe for
    // making one lives in the server reconcile (IDB sub_25568A0, 0x2556FA0..
    // 0x255717B) - it creates a stack from nothing using these primitives. We
    // replay it verbatim; every step below was live-validated 2026-07-15 (a real,
    // usable, persistent item that survives save/reload):
    //
    //   container = *(holder+8)                       // kOff_InvHolder_Container
    //   tag       = *(*(container+0x88)+1)            // object-type tag
    //   owner     = (tag & 0xF7) ? *(container+0xA0) : container
    //   alloc     = *(*(owner+0x68)+0x10)             // instance-id allocator
    //   ctor(itemVal, &typeId, qty)                   // kSig_TrItemValueCtor
    //   itemVal+0x0A = 0
    //   itemVal+0x00 = InterlockedIncrement64(alloc+0x20)   // THE unique id
    //   bucket = the holder bucket whose +0x10 == itemDef+66
    //   plan(bucket, &err, container, {itemVal,1,1}, 0, &out, 0, 0, 1)
    //   for each 216-byte placement p in out:
    //       commit(holder, &err, 0, p, *(u16*)(p+208))
    //   freePlacements(&out); dtor(itemVal)
    //
    // WHY EACH PIECE MATTERS (each was a separate failed attempt):
    //  * The PLAN alone mutates nothing - it deep-copies the bucket and emits
    //    placement records. Calling it and seeing err=0 means nothing. The
    //    COMMIT is what writes, via sub_ED65670 (the only function in the whole
    //    chain that touches the live slot array).
    //  * The unique instance id is not optional. An earlier design memcpy'd a
    //    template slot and stamped typeId+qty; the item had no id and it BRICKED
    //    A SAVE. Never fabricate a slot - always go through ctor + allocator.
    //  * The REALM must match the target holder (see kTls_RealmFlag): the ctor
    //    reaches sub_ED69660, which reads the client-only storage base
    //    qword_6180EB0 and forces it to 0 in the server realm, so a client-realm
    //    item is subtly different from a server-realm one.
    //  * BOTH holders must be written, each in its own realm, sharing ONE
    //    allocator id - the same rule the quantity editor already follows. The
    //    server->client reconcile syncs quantities but does NOT create slots, so
    //    a server-only add is invisible until a save/reload.
    //
    // Buffer note: the ctor does NOT write every byte of the 192 (+0x0C, +0x3A,
    // +0x54, +0x8A.. are left untouched). The game gets away with an
    // uninitialised stack buffer because it copy-constructs before the value
    // goes anywhere; we hand ours straight to the planner, so ZERO IT FIRST or
    // the holes reach the live slot (live-seen: garbage at +0x0C).

    // TrItemValue ctor (IDB sub_1F86FD0): void f(itemVal, u16* typeId, i64 qty).
    // Self-contained - fills subtype/durability/flags/sub-lists from the item
    // def alone. Leaves the instance id as -1 for the caller to stamp.
    // In 1.17.00 the prologue alone matches TWO functions and FindPattern
    // returns the lower address - which is not this one, so the add path was
    // calling an unrelated function with (itemVal, u16*, i64) and faulting on
    // its first instruction (logged as "exception (built=0 planned=0)").
    // Extended through the ctor's own opening writes, which are its identity
    // and match the documented prototype exactly:
    //     mov qword ptr [rcx], -1        ; instance id, left for the caller
    //     movzx eax, word ptr [rdx]      ; typeId  (arg2, u16*)
    //     mov word ptr [rcx+8], ax
    //     mov qword ptr [rcx+0x10], r8   ; quantity (arg3, i64)
    // Unique.
    inline constexpr const char* kSig_TrItemValueCtor =
        "48 89 5C 24 ?? 48 89 4C 24 ?? 55 56 57 41 54 41 55 41 56 41 "
        "57 48 8B EC 48 83 EC 70 4C 8B F2 4C 8B E1";
    // NOTE on destination storage. The holder also carries a live item->storage
    // map (0x1420825C0, a jmp thunk into 0x14E0E30C0), and the per-placement
    // insert at 0x142077410 consults it before falling back to the definition's
    // default. It is tempting to match that here - but the engine's own add
    // path does NOT: at 0x142A6FD3B it takes the default from itemInfo+0x428
    // and plans against that bucket, leaving the insert free to re-derive.
    // Planning against the map instead makes Trinity disagree with the
    // engine's planner rather than agree with its insert. Use the default.

    // Publish that the holder changed (0x142A93B00 in 2760):
    //     void f(void* holder)
    // Appends to the revision vector at holder+0xF8, rolls the sync hash and
    // clears the local-modification flag at holder+0x100. The engine calls it
    // after every add (0x142A6FF8E) and it is how subsystems caching a view of
    // the container - the equipment component's ammo watcher included - learn
    // to look again.
    //
    // Safe to call blind: it compares holder+0x100 against holder+0x108 on
    // entry and takes a different path when they disagree, so it will not
    // append a revision the holder is not ready for. Unique.
    inline constexpr const char* kSig_InvBumpRevision =
        "40 57 48 83 EC 20 8B 81 08 01 00 00 48 8B F9 39 81 00 01 00 "
        "00 0F 85";

    // Per-placement COMMIT (0x142077410 in 2760):
    //     int* f(void* holder, int* outErr, void* placement, u16 slotIdx)
    // Re-finds the bucket from the item's own definition and calls the inserter,
    // which validates the item may live in that storage, copies it into an empty
    // slot (or merges onto an existing stack) and maintains the used-slot count.
    //
    // This is the function the engine's OWN plan-then-commit loop uses. At
    // 0x142A6FE30, straight after the same planner Trinity calls with the same
    // arguments, it walks the placement vector by 0xE0 and does exactly:
    //     movzx r9d, word ptr [rdi+0xD8]   ; the slot the planner chose
    //     mov   r8, rdi                    ; the placement itself
    //     lea   rdx, [rbp+0xC]             ; error out
    //     mov   rcx, r14                   ; the holder
    //     call  0x142077410
    // - which also confirms kOff_Placement_SlotIdx independently.
    //
    // Do NOT confuse this with 0x14207A2C0, which the 2760 re-derivation
    // briefly bound this name to. That one takes eight arguments and means
    // something else: all three of its callers fetch the placement out of a
    // LIVE slot first (via 0x142078A70), so it commits a CHANGE to a slot that
    // is already populated. A slot the planner has merely reserved is not, and
    // it refused every add for that reason. The giveaway was that the refusal
    // was byte-identical for every item and both realms - a wrong argument
    // varies with the item, a wrong function does not.
    // Unique.
    inline constexpr const char* kSig_InvCommitPlacement =
        "48 89 5C 24 ?? 48 89 6C 24 ?? 56 57 41 56 48 83 EC 30 41 0F "
        "B7 58 08 48 8B F1";
    // Free the planner's placement vector (IDB sub_7D13B10, reached via the
    // 5-byte jmp thunk sub_332C40 - thunks cannot be signatured, so this is the
    // target; calling it is identical). Its `imul rcx, rax, 0E0h` in the
    // signature below IS the 224-byte placement stride - a nice self-check.
    // MUST start at the real prologue: the function saves rbx and pushes rdi
    // before `sub rsp, 20h`, and its epilogue still runs `pop rdi; ret`.
    // Entering at the `sub` (as this signature used to) makes that `pop` eat
    // the return address and `ret` jump to a stack value - which is exactly
    // the "faulting module: unknown" access violation Add Item was dying on.
    // Extended through the `imul rcx, rax, 0E0h` itself. The shorter prefix was
    // unique in 2760 and matched TWICE in 2850: the second hit, 0x148F67630, is
    // DestroyEquippedItemBehavior's vector free, byte-identical up to the stride
    // immediate and then 0xF0 where this one is 0xE0. The stride self-check
    // below does catch it - 240 != 224 disables Add Item rather than corrupting
    // anything - but a signature that needs a later guard to notice it matched
    // the wrong function is already wrong. Carrying the stride inside the
    // pattern makes the two impossible to confuse. Verified: one match in 2850.
    inline constexpr const char* kSig_InvFreePlacements =
        "48 89 5C 24 ?? 57 48 83 EC 20 48 89 CB 48 83 39 00 74 57 31 "
        "FF 39 79 08 76 ?? 66 0F 1F 44 00 00 89 F8 48 69 C8 E0 00 00 00";
    // Byte offset of that `imul` immediate inside the match. We re-read it at
    // load and refuse Add Item unless it agrees with kPlacement_Stride - the
    // stride moving under us is precisely how a placement loop would start
    // writing into the wrong slot.
    inline constexpr uintptr_t kOff_FreePlacements_StrideImm = 37;
    // Byte offset of freePlacements' `call <TrItemValue dtor>` - the per-element
    // destructor it runs over the placement vector. The game calls that SAME
    // function on the item it constructed (0x1426A198F), so resolving the dtor
    // from here is exact, where a byte signature for it matched a different
    // class whose +0xC0 is a refcount rather than an owned pointer.
    inline constexpr uintptr_t kOff_FreePlacements_ElemDtorCall = 0x31;

    inline constexpr uintptr_t kOff_InvHolder_Container = 0x08; // holder+8 -> container
    // ItemInfo._defaultPushInventoryInfo - which storage this item goes to by
    // default. The commit re-reads it, but we need it to pick the bucket too.
    // 1.17.00: +0x418.
    // 1.18.02 (1.0.0.2625): moved +0x10 to +0x428 (confirmed from iteminfo caller @ 0x150221716:
    //     movzx r8d, word ptr [rax + 0x428]
    inline constexpr uintptr_t kOff_ItemDef_BucketType  = 0x428; // u16
    //
    // ★ The inventory CONTAINER *is* the player CHARACTER object - the very same
    // "owner" the player/god-mode code resolves. Live-confirmed 2026-07-15: the
    // container's type tag (kOff_Owner_TypeDesc -> +1) reads 1 = SelfPlayer, and
    // its possessor round-trips exactly as kOff_Owner_Possessor/
    // kOff_Possessor_Pawn describe. So the inventory and player systems are the
    // same object graph, and those constants are reused here rather than
    // redefined - see their comment above for why the round-trip is the true
    // single source of truth.
    //
    // What is new: each REALM has its OWN player character (client and server
    // hold different characters, with different possessors), and BOTH round-trip
    // - each is the live character of its own realm. That is precisely what the
    // add path needs, since it must find the live store on each side.
    //
    // It also means the round-trip is the only way to tell the live store from
    // the insert planner's short-lived DEEP COPIES of it: a copy carries the
    // original's possessor pointer, and a possessor can only point back at one
    // character. Copies mirror the player's contents ~99% and match on bucket
    // count and type tag, so nothing else rejects them - and they are freed,
    // so mistaking one for the real store means faulting on a dead holder.
    inline constexpr uintptr_t kOff_Sub_IdAllocator = 0x10; // (owner+0x68)+0x10 -> id allocator
    inline constexpr uintptr_t kOff_IdAlloc_Counter = 0x20; // i64, InterlockedIncrement64 target

    // The live slot is 0xC8, but the constructor/planner work object is 0x108.
    // The extra tail is part of the transaction ABI recovered from the
    // previously working add path; truncating it trips /GS before an engine
    // error can be reported.
    inline constexpr uintptr_t kItemVal_Size        = 0x108;
    inline constexpr uintptr_t kOff_ItemVal_InstanceId = 0x00; // i64 (-1 out of the ctor)
    inline constexpr uintptr_t kOff_ItemVal_Subtype    = 0x0A; // u16 (reconcile zeroes it)
    // 1.17.00 grew the placement record by 8 bytes (216 -> 224) and moved the
    // trailing slot index with it (+208 -> +216). Both were read straight out
    // of the game's own commit loop, not guessed.
    inline constexpr uintptr_t kPlacement_Stride       = 224;
    inline constexpr uintptr_t kOff_Placement_SlotIdx  = 216;  // u16

    // --- The client/server realm flag ----------------------------------------
    // The engine runs two realms in one process and selects between them with a
    // per-thread flag, inlined everywhere as:
    //     root = qword_6180F60; if (*(u8*)(TLS+498)) root = qword_6180F68;
    // where TLS = *(NtCurrentTeb()->ThreadLocalStoragePointer).
    // Item construction is realm-sensitive through it (see the add-item note
    // above), so building an item for the server holder means flipping this for
    // the duration and restoring it afterwards - leaving a game thread in the
    // wrong realm would corrupt whatever it touches next.
    //
    // Get the TEB via NtQueryInformationThread(ThreadBasicInformation): a plain
    // exported ntdll call. Do NOT hand-roll a `mov rax, gs:[30h]` stub - that
    // was tried and fails (bogus TEB, then an access violation on the second
    // call, almost certainly CFG rejecting an indirect call into our own page).
    inline constexpr uintptr_t kOff_Teb_TlsPointer = 0x58; // TEB.ThreadLocalStoragePointer
    inline constexpr uintptr_t kTls_RealmFlag      = 509;  // u8: 0 = client, 1 = server (was 498 before 2.01.00)

    // Item-info table (typeId -> item definition -> item key string, for names).
    // Its resolver is one of ~121 identical 16-bit-key table-resolver clones, so
    // - like the fast-travel/area-name tables above - it is located by string-
    // anchoring on its unique table name "iteminfo": find the string, find the
    // `lea r8,[rip+str]` that passes it, scan back to the clone prologue, and
    // read its RIP-relative `mov rbx, cs:<table global>`. The clone prologue
    // here differs from kSig_TableResolverPrologue only in loading a 16-bit key
    // (0F B7 39) instead of 32-bit (8B 39); the mov-global sits at +0x15.
    //   table +0x08 u32  count (typeId bound)
    //   table +0x50 ptr  def[]      (def = *(table+0x50 + 8*typeId))
    //   def   +0x08 ptr -> string object whose first qword is the key char*
    inline constexpr const char* kStr_ItemInfoTable = "iteminfo";
    inline constexpr uintptr_t kOff_ItemResolver_MovGlobal = 0x15;
    inline constexpr uintptr_t kOff_ItemTable_Count = 0x08; // u32
    // 1.17.00 moved the row array one qword later (0x50 -> 0x58); the count
    // at +0x08 did not move. Read out of the resolver clones themselves, which
    // do `mov rax,[table+0x58]; mov rax,[row*8 + rax]` - all five resolvers we
    // can reach by string anchor agree.
    inline constexpr uintptr_t kOff_ItemTable_Defs  = 0x58; // ptr[]
    inline constexpr uintptr_t kOff_ItemDef_Key     = 0x08; // ptr -> string obj

    // --- Storages: what each bucket IS ---------------------------------------
    // "InventoryInfo" is the table describing every storage in the game - one
    // row per InventoryType, and bucket+0x10 (above) is that row's _key. Its
    // rows are what let the menu name a storage without hardcoding anything.
    // The 20 storages, by engine key (the game registers exactly these):
    //     Money, Character (your pack), PearlUser, PearlCharacter, Quest,
    //     PetAndVehicle, Wagon, CampWareHouse, WareHouse, Bank, CampStraw,
    //     BirdFeed, Recovery, Housing_Dresser, Housing_Refrigerator,
    //     Housing_Symbol, Housing_Collecting, Housing_GatheredMaterials, Kuku,
    //     InvisibleInventory
    // Which key maps to which NUMBER is deliberately not recorded here: the
    // order those statics are declared in is NOT the enum value (live rows come
    // back in an order that contradicts it), and nothing needs to know - we read
    // the number off the bucket and let the table say what it is. Do not
    // reintroduce a hardcoded ordinal list; it would be guesswork.
    //
    // It is another 16-bit-key resolver clone (IDB sub_516050, 176-byte rows,
    // row loader sub_517460, deserializer sub_1171F30) and shares the table
    // layout above (+0x08 count, +0x50 def[]), so DefForRow walks it unchanged.
    //
    // The ONE difference that matters: this clone loads its table-name string
    // INDIRECTLY - `mov r8, cs:<slot>` where the slot holds a char* to
    // "Inventory" - rather than `lea r8, "iteminfo"`, because that name is a
    // shared/interned string. Same clone shape otherwise (the name load sits at
    // fn+0x4E either way), so the only change needed was to let the table hunt
    // follow the extra indirection. Verified unique: exactly one site in the
    // image both loads this name pointer AND has the clone prologue above it;
    // the only other site is the table loader, which the prologue check already
    // rejects - the same discriminator "iteminfo" relies on.
    inline constexpr const char* kStr_InventoryInfoTable = "Inventory";
    inline constexpr const char* kSig_MovR8Rip = "4C 8B 05 ?? ?? ?? ??";
    //
    // InventoryInfo fields we consume (field->offset recovered from the
    // deserializer's own per-field error strings, exactly as for ItemInfo):
    inline constexpr uintptr_t kOff_InvDef_Key      = 0x08; // _stringKey -> "WareHouse"
    // _InventoryNameUIText is the game's own label, but it is NOT unique: several
    // rows share "Inventory", because in the game's own screens the surrounding
    // UI says which one you are looking at (the warehouse has both a "focus
    // Inventory" and a "focus WareHouse" pane). A flat list has to qualify the
    // repeats itself - see the storage naming in inventory.cpp.
    inline constexpr uintptr_t kOff_InvDef_Name     = 0x70; // _InventoryNameUIText (loc-string)
    inline constexpr uintptr_t kOff_InvDef_DefSlots = 0x48; // u16 _defaultSlotCount
    inline constexpr uintptr_t kOff_InvDef_MaxSlots = 0x4A; // u16 _maxSlotCount
    // _InventoryNameUIText is built by the SAME builder as ItemInfo._itemName
    // (IDB sub_FF6460), so the localised-name walk below reads it verbatim.

    // --- Item taxonomy: the inventory's REAL category tree -------------------
    // Every "*info" table row is built by a GENERATED deserializer whose
    // per-field failure message names the C++ member it was reading, e.g.
    //   "ItemGroupInfo의 _groupName를 읽어들이는데 실패했다."
    //     ( = "failed to read ItemGroupInfo's _groupName" )
    // Walking that deserializer's disassembly and pairing each field read with
    // the error string in its failure block recovers the field->offset map
    // EXACTLY - no guessing. (IDB: ItemInfo = sub_1177130, 1016-byte row;
    // ItemGroupInfo = sub_11782F0, 120-byte row.) This is a general technique
    // for this binary; reuse it for any table.
    //
    // ItemInfo fields we consume (offsets into the def resolved above):
    inline constexpr uintptr_t kOff_ItemDef_Name   = 0x20;  // _itemName (loc-string struct)
    inline constexpr uintptr_t kOff_ItemDef_Groups = 0x350; // _itemGroupInfoList (vector)
    inline constexpr uintptr_t kOff_ItemDef_Tier   = 0x210; // u8 _itemTier (rarity 0..5)
    // _maxStackCount / _applyMaxStackCap: the per-item stack cap. Most rows
    // already carry 999999 (an effectively-unlimited default) - the cap that
    // actually bites for outliers (equipment, key items, some consumables) is
    // whatever _applyMaxStackCap gates. Inventory::SetAllMaxStackSizes forces
    // both to one uniform value/enabled across every row in one pass.
    inline constexpr uintptr_t kOff_ItemDef_MaxStackCount    = 0x18;  // i64
    inline constexpr uintptr_t kOff_ItemDef_ApplyMaxStackCap = 0x111; // u8 bool
    //
    // "ItemGroupInfo" (note the capitals - it is NOT lowercase like "iteminfo")
    // is the inventory's category tree, and it is what the UI actually shows.
    // Its resolver is another 16-bit-key clone, byte-identical in prologue to
    // the iteminfo one, so the same string-anchor finds it and the table layout
    // is shared (+0x08 count, +0x50 def[]). ItemInfo._itemGroupInfoList holds
    // ROW INDICES into it - that is the item -> category link.
    //   grpDef +0x08 ptr _stringKey ("ItemGroup_SubCategory_Equip_Weapon_Range")
    //   grpDef +0x18     _groupName  <- LOCALISED display text ("Ranged Weapon")
    //   grpDef +0x38 vec _itemGroupInfoList (child groups)
    //   grpDef +0x48 vec _itemInfoList
    //   grpDef +0x68 u16 _orderIndex
    //   grpDef +0x6C u16 _iconPath
    //   grpDef +0x6E u8  _isShowCategoryString
    //
    // _orderIndex alone encodes the whole hierarchy (live-verified across a
    // real inventory - every item resolves to exactly this shape):
    //     1..5    the top tabs      (Equipment, Food, Materials, Documents, Others)
    //     1300..8900 on a decade base = the sub-category the game displays
    //                (1500 "Two-Handed Weapon", 1600 "Ranged Weapon",
    //                 5100 "Elixir", 6600 "Crafting and Refinement Material",
    //                 7000 "Currency", 7800 "Projectile", 8300 "Enhanced Kuku Pot")
    //     base+n  leaf groups, which always sort INSIDE their own sub-category
    //             (1509 "Two-Handed Sword" < 1600 "Ranged Weapon")
    //     65535   internal semantic groups ("Material_Alchemy_HP_Main_Tier2")
    //             that must never be displayed - the sentinel is what tells
    //             display rows apart from bookkeeping rows.
    // So: drop 65535, take <=5 as the tab (OPTIONAL - e.g. Lubricant/Cogwheel
    // have no tab, only Currency), and the smallest remaining order is the
    // category. Nothing about this is hardcoded, and it tracks game updates.
    inline constexpr const char* kStr_ItemGroupInfoTable = "ItemGroupInfo";
    inline constexpr uintptr_t kOff_GrpDef_Name    = 0x18; // loc-string struct
    inline constexpr uintptr_t kOff_GrpDef_Order   = 0x68; // u16 _orderIndex
    inline constexpr uint16_t  kGrpOrder_Internal  = 0xFFFF; // never displayed
    inline constexpr uint16_t  kGrpOrder_MaxTopTab = 5;      // 1..5 = top tabs
    // Engine vector shape shared by the *List fields above.
    inline constexpr uintptr_t kOff_Vec_Data  = 0x00; // T*
    inline constexpr uintptr_t kOff_Vec_Count = 0x08; // u32

    // --- Icons: the game's own sprite names, for items AND categories --------
    // ItemInfo._itemIconList (+0x90) is the same vector shape, over 32-byte
    // ItemIconData rows (deserializer sub_1196CB0, fields named by its error
    // strings exactly as above):
    //     +0x00 u16 _iconPath      +0x02 u16 _highlightIconPath
    //     +0x04 u8  _checkExistSealedData
    //     +0x08 vec _gimmickStateList      +0x18 u8 _checkUsable
    // _iconPath is NOT text - it is a u16 ROW INDEX into the `stringinfo`
    // table. (Its field reader, sub_1187F20, maps the on-disk u32 key to a row
    // index through the table's key map at +0x60; the u16 it leaves behind is
    // that row.) ItemGroupInfo._iconPath (+0x6C) goes through the SAME reader,
    // so a category's icon is the identical walk - which is why one code path
    // serves both.
    //
    // stringinfo is another 16-bit-key resolver clone (IDB sub_304590, row
    // loader sub_3069F0), byte-identical in prologue, so the shared table
    // layout (+0x08 count, +0x50 def[]) and the same string-anchor find it:
    //     +0x00 u32 _key    +0x08 _stringKey (EMPTY on the icon rows)
    //     +0x10 u8  _isBlocked
    //     +0x18     _buffer   <- the payload: the icon sprite name
    // The sprite name lowercased + ".dds" is the icon's file in pak chunk 0012
    // under ui/texture/icon. Live-verified: item 6476 "Righteous Verdict" ->
    // "ItemIcon_Prefab_cd_phm_02_sword_0039" -> itemicon_prefab_cd_phm_02_sword_0039.dds,
    // and category "Two-Handed Weapon" -> "ItemIcon_ItemGroup_twohand_weapon"
    // -> itemicon_itemgroup_twohand_weapon.dds (71 such category icons ship).
    // Note the strings' casing is inconsistent in the data ("itemIcon_...",
    // "ItemIcon_...") - lowercase before using one as a file name.
    //
    // The resolver has a lazy row-load path, but every row an item points at is
    // already populated at load (live-checked: 38 of 38 items, zero nulls), so
    // this stays a pure pointer walk and never calls into the game.
    inline constexpr const char* kStr_StringInfoTable = "stringinfo";

    // --- Wanted level -------------------------------------------------------
    // "WantedInfo" is a standalone string referenced by three lea-r8 sites, the
    // same shape every other table resolver anchors on.
    //
    // _increasePrice is how much a crime adds to your bounty. Its offset came
    // from the engine's own deserialiser: the Korean parse-failure message
    // naming WantedInfo::_increasePrice is referenced from one place, and the
    // read it belongs to is `lea rdx,[rdi+0x18]` with a size of 8. Zeroing it
    // on every row means no crime raises the bounty - the game keeps working
    // exactly as before, it just adds nothing.
    inline constexpr const char* kStr_WantedInfoTable = "WantedInfo";
    inline constexpr uintptr_t kOff_WantedDef_IncreasePrice = 0x18; // i64
    // The one field in the row not yet accounted for. Zeroing the price left the
    // crime still registering - poster and region marker - so this is tried as
    // an explicit experiment rather than a claim: same table, same session-only
    // reach, fully restored on disable. If it changes nothing it costs nothing.
    inline constexpr uintptr_t kOff_WantedDef_IsBlocked     = 0x10; // u8
    inline constexpr uint32_t  kWantedRows_Max = 4096; // sanity bound; the live table is ~35

    // Zeroing the price left the crime registering: poster and region marker
    // still appeared, and _isBlocked on the WantedInfo row turned out not to be
    // the gate either. What decides that hitting a faction's people IS a crime
    // is one byte on the TRIBE: TribeInfo::_wantedCrimeType.
    //
    // Offset read the same way as _increasePrice - from the deserialiser's own
    // parse-failure message, whose read is `lea rdx,[rdi+0x50]` with size 1.
    // Setting it to 0 removes the crime type, so there is nothing to register
    // rather than something registered as worthless.
    inline constexpr const char* kStr_TribeInfoTable = "tribeinfo";
    inline constexpr uintptr_t kOff_TribeDef_WantedCrimeType = 0x50;
    // The value that means "this tribe reports nothing". It is 7, not 0.
    //
    // Zero is an ordinary member of the enum, and writing it was writing a
    // crime type rather than clearing one - the consumer at 0x14251D12A tests
    // for 7 first and branches straight out, then compares the remaining
    // values individually (`cmp cl, 5; sete al`). So the tribe half of No
    // Bounty never worked; on 2760 it merely looked busy, because the data
    // happened to be non-zero and Trinity changed 372 rows to a different
    // crime. On 2850 the rows already read 0, so it changed nothing and
    // finally said so.
    inline constexpr uint8_t   kTribeCrime_None              = 7; // u8

    // The gate every crime passes through, and the right place to stop one.
    //
    //   bool f(void* actor, void* target, void* entity, const void* tag,
    //          uint8_t category)     // category is the 5th arg, at [rbp+0x50]
    //
    // Editing WantedInfo and TribeInfo - what No Bounty did until now - only
    // ever zeroed the PRICE a crime costs. It could not stop the crime from
    // happening, and for theft it could not even reach: at 0x14251D107 the
    // function does `cmp dil, 7; jae`, so category 7 (take-or-steal ownership)
    // jumps clean over the TribeInfo lookup at 0x14251D125 the table edit was
    // aiming at. Guards still turned hostile and NPCs still fled; only the
    // bounty number stayed at zero.
    //
    // Returning false here refuses the crime outright - theft, assault and
    // trespass alike - and mutates no shared definition data, so it toggles
    // instantly and leaves nothing to roll back.
    //
    // Frame size wildcarded deliberately: the literal `48 81 EC 80 00 00 00`
    // is also unique today, but a stack frame is the compiler's to resize and
    // 2.01.00 broke thirty signatures doing exactly that. Verified: one match
    // either way, at 0x14251CFF0.
    inline constexpr const char* kSig_CrimeGate =
        "48 89 5C 24 ?? 48 89 74 24 ?? 48 89 7C 24 ?? 55 41 54 41 55 "
        "41 56 41 57 48 8B EC 48 81 EC ?? ?? ?? ?? 49 8B F0 4C 8B F2 "
        "4C 8B E9 0F B6 7D 50 40 88 7C 24 ??";
    inline constexpr uint32_t  kTribeRows_Max = 8192;
    inline constexpr uintptr_t kOff_StrDef_Buffer  = 0x18; // ptr -> string obj
    inline constexpr uintptr_t kOff_ItemDef_Icons  = 0x90; // _itemIconList (vector)
    inline constexpr uintptr_t kOff_IconData_Path  = 0x00; // u16 -> stringinfo row
    inline constexpr uintptr_t kOff_GrpDef_Icon    = 0x6C; // u16 -> stringinfo row
    inline constexpr uint16_t  kIconPath_None      = 0xFFFF;

    // --- Category icons the game ships but never names ------------------------
    // A handful of displayed categories have NO usable _iconPath: the sprite
    // name they would need is simply absent from `stringinfo`, so no row can
    // point at it. "Packaged Trade Goods" is one - stringinfo carries
    // "ItemIcon_ItemGroup_trade" but nothing for trade_packed - yet
    // itemicon_itemgroup_trade_packed.dds IS in pak chunk 0012 all the same.
    // The art exists; only the data link is missing.
    //
    // We load icons from the pak BY FILE NAME and never touch stringinfo, so we
    // can name the file ourselves. The group's _stringKey gives it away, because
    // the two follow one convention:
    //     ItemGroup_SubCategory_trade_Packed -> ItemIcon_ItemGroup_trade_Packed
    //                                        -> itemicon_itemgroup_trade_packed.dds
    // Checked against the shipped data: of the 50 SubCategory rows, 43 derive to
    // a .dds that exists. That is a rule, not a coincidence - and a derived name
    // that happens to miss just draws blank, exactly as the row does today.
    inline constexpr uintptr_t   kOff_GrpDef_Key      = 0x08; // ptr -> string obj
    inline constexpr const char* kGrpKey_SubCatPrefix = "ItemGroup_SubCategory_";
    inline constexpr const char* kIconPrefix_ItemGroup = "ItemIcon_ItemGroup_";
    // Last resort, for a category we can name no icon for at all - chiefly our
    // synthetic "Uncategorised" bucket, which is not a game row and so has no
    // key to derive from. This is the game's own "unknown category" art, and it
    // ships. It pairs with the "Uncategorised" label fallback: a row that falls
    // back on one falls back on the other.
    inline constexpr const char* kIcon_Uncategorised  = "ItemIcon_ItemGroup_special_unknown";

    // --- Localised display names (real in-game text) -------------------------
    // ItemInfo._itemName is a 32-byte localised-string struct; its first qword
    // is a "provider" object whose vtable slot 3 is the text getter. That getter
    // (IDB sub_FF6430) is only a bounds-checked pointer walk, so we replicate it
    // as guarded reads instead of calling into the game from the render thread:
    //     off  = *(u32*)(provider + 0x10);          // 0xFFFFFFFF until interned
    //     blob = *(void**)(locMgr + 0x08);          // +0x00 data, +0x08 u32 size
    //     name = off < size ? *(char**)blob + off : "";
    // The bounds check is what makes this safe: an unresolved (-1) offset can
    // only ever yield "", never a wild pointer. The blob is a ~9.8MB interned
    // char pool holding the CURRENT language's text, which is why this beats a
    // baked-in name table (the community one this replaced had gone stale - it
    // mapped Money_Copper to "Silver"; the game says "Copper").
    // Anchored on the getter itself: 0x22 bytes, unique, and its
    // `mov rax, cs:<locMgr>` sits at +0x03 (7-byte instruction).
    // The provider field displacement (the `10` in `mov edx,[rcx+10h]`) is
    // wildcarded: game 1.17.00 moved it to +0x18. We read the real byte out of
    // the matched instruction at load instead of pinning either number, so a
    // future reshuffle of the provider struct costs nothing.
    // 1.18.0 inlined the blob into the manager, so the getter lost a hop:
    //   old  mgr -> blob = [mgr+8]; size = [blob+8]; data = [blob+0]
    //   new  size = [mgr+0x68];     data = [mgr+0x60]
    // Nothing about the old shape could match that, which is why wildcarding
    // never found it. Every displacement below is read out of the matched code
    // at load rather than pinned.
    inline constexpr const char* kSig_LocStringGet =
        "8B 41 ?? 48 8B 0D ?? ?? ?? ?? 3B 41 ?? 72 08 "
        "48 8D 05 ?? ?? ?? ?? C3 48 03 41 ?? C3";
    inline constexpr uintptr_t kOff_LocGet_MovGlobal = 0x03; // mov r64, cs:<locMgr>
    // Byte positions of the three displacements inside the match, all read at
    // load. Fallbacks are 1.18.0 values and only used if a read fails.
    inline constexpr uintptr_t kOff_LocGet_ProvDisp  = 0x02; // mov eax,[rcx+XX]
    inline constexpr uintptr_t kOff_LocGet_SizeDisp  = 0x0C; // cmp eax,[rcx+XX]
    inline constexpr uintptr_t kOff_LocGet_DataDisp  = 0x1A; // add rax,[rcx+XX]
    inline constexpr uintptr_t kOff_LocProv_Offset   = 0x18; // fallback
    // Fallbacks only: Install() reads the live values straight out of the
    // getter's own instructions (kOff_LocGet_SizeDisp / _DataDisp) and
    // overwrites these. They had been stale since before TU 2.02.00 without
    // anyone noticing, precisely because the self-resolve always won - the
    // 2.01.00 logs already printed "size +0x60, data +0x58" against constants
    // reading 0x68/0x60. Corrected against 0x14123262A `cmp eax, [rcx+0x60]`
    // and 0x141232637 `add rax, [rcx+0x58]`, so the fallback is now worth
    // falling back to.
    inline constexpr uintptr_t kOff_LocMgr_Size      = 0x60; // fallback
    inline constexpr uintptr_t kOff_LocMgr_Data      = 0x58; // fallback
    // --- Crime / stealth: research only, no signatures kept ------------------
    // Eight signatures used to live here, covering the theft-and-witness path.
    // Nothing ever called them: No Bounty is implemented by zeroing the
    // WantedInfo data table (Inventory::SetNoBounty), not by hooking any of
    // this. They were removed in the 2.01.00 re-derivation, for a reason worth
    // recording: an unused signature is not free. Every patch "breaks" it, it
    // is counted as damage, and someone then spends an afternoon re-deriving a
    // function that no code will ever call. Eight of the thirty signatures
    // 2.01.00 broke were these.
    //
    // The route back, if a crime feature is ever built, is the part worth
    // keeping. All of it was found from the class names, which the engine
    // stores as plain strings and which survive recompiles far better than any
    // byte pattern:
    //
    //   ClientStealItemInteractionProcessor  execute() is vtable[2]; a
    //       separate step/process function drives it
    //   AIFunction_RegistCrime               execute() - the AI-side registrar
    //   AIFunction_WitnessCriminalPlayer     execute() - near-identical to the
    //       registrar; in 2658 the two differed by a single jump displacement,
    //       so any pattern must be checked against BOTH before it is trusted
    //   TrocTrWantedAddCrimeRecordReq        records pickpocket/assault/steal
    //   TrocTrSetCrimeTargetReq              marks the player as a crime target
    //
    // A stealth-report function and a theft/witness broadcast sit on the same
    // path. The wanted-level data table above (kStr_WantedInfoTable and the
    // WantedDef offsets) is still live and still used - it is what No Bounty
    // actually writes.
    inline constexpr uintptr_t kOff_WantedDef_UseTargetPrice = 0x20;

    // --- World: Master Frame Update (Game Speed / Timescale) -----------------
    // The engine's root frame updater. Its context carries the TimeManager at
    // +0x60; setting TimeManager.mode (+0x50) = 1 and TimeManager.timeScale
    // (+0x54) = mult drives all animation, physics, AI and combat simulation.
    // Confirmed in 2760 at 0x140A541C0: the engine's own consumer a few
    // hundred bytes in reads exactly those fields - `movzx ecx,[rax+0x50];
    // cmp cl,1; vmovss xmm1,[rax+0x54]` - and scales [rax+0x64] by it.
    //
    // This is matched INSIDE the function, not at its prologue, and installed
    // with InstallHookInterior. 2.01.00 is why: the function did not move or
    // change behaviour, but its register saves and stack frame were
    // recompiled (rsi pushed rather than stored, r12 added, 0x1D0 -> 0x1B0),
    // and that alone killed the old prologue pattern and Game Speed with it.
    // The prologue is still not unique in 2760 - five functions share its
    // shape - whereas the four instructions below are unique image-wide:
    //
    //     mov rdi, rcx            the incoming context
    //     mov rdx, [rcx+0x60]     -> TimeManager
    //     mov eax, [rdx+0x64]
    //     mov [rdx+0x60], eax     carry last frame's value forward
    //
    // Every constant in it is a struct offset, which is fixed by the data
    // layout rather than by the compiler. Unique match (1) at 0x140A54203.
    inline constexpr const char* kSig_MasterFrameUpdate =
        "48 8B F9 48 8B 51 60 8B 42 64 89 42 60";

    // --- World: Game Speed (fixed-timestep override) ------------------------
    // The engine's per-frame timing update (IDB sub_8FBD80) measures the real
    // frame delta, applies UI/pause/native-timescale factors, and stores the
    // master delta the whole simulation (animation, physics, AI, ability
    // timers) advances by. Its tail carries a FIXED-TIMESTEP override, used by
    // the game's own video/demo capture so recorded frames are smooth and
    // deterministic regardless of render rate:
    //     if (byte_606B9CE == 1)                 // enable flag
    //         frameDelta = dword_615A4F0;        // forced seconds-per-frame
    // Capture start (IDB sub_34B35D0) sets dword_615A4F0 = 1.0f/targetFps
    // (default 60 -> 0.0166667) and flips the flag on; capture stop
    // (sub_34B3950 / sub_3635600) clears it. Nothing in the frame loop clears
    // the flag, so writing it ourselves sticks (live-confirmed by the user).
    //
    // Game Speed is the engine's OWN time scale, set on the time manager
    // that hangs off the master frame update's ctx+0x60: a mode byte at
    // +0x50 (1 = scaled) and a multiplier float at +0x54. The engine reads
    // both at 0x1409481CE/0x1409481D7 and applies them itself:
    //     vmulss xmm0, xmm1, [rax+0x64]   ; 0x1409481F9, the frame delta
    //     vmulss xmm0, xmm1, [rax+0x68]   ; 0x140948207, and its partner
    // so world.cpp only has to write the two fields - see hkMasterFrameUpdate.
    //
    // There is no signature here on purpose. The older approach forced the
    // delta directly through two BSS globals located by a pattern over the
    // engine's own override block, and NOP-patched that block's one-shot
    // guard. It resolved correctly on 2658 and was still wrong: it wrote
    // [timeMgr+0x64] LATER in the same function than the scaling above, so
    // it overwrote the multiplier with a fixed 1/60 s step every frame.
    inline constexpr uintptr_t kOff_TimeMgr_Mode       = 0x50; // u8: 1 = scaled
    inline constexpr uintptr_t kOff_TimeMgr_Multiplier = 0x54; // f32
    // --- Time of Day: the master field clock (World feature, world.cpp) -------
    // The REAL day/night clock is two BSS globals (client / server realm), each
    // a 32-byte struct of int32s. The per-frame sun/sky update reads them (IDB
    // sub_1CA3890 -> sub_871360) and writes them (IDB sub_8719B0 / sub_1D44970).
    // Everything else about time-of-day is downstream: the "TimeOfDayManager"
    // and its +0x3D0 "currentTimeOfDay" float are a RENDER MIRROR that nothing
    // reads back (writing it is a no-op, live-confirmed), and the engine
    // timeScale is a GLOBAL sim scale that freezes the whole world - both were
    // dead ends. These two globals are the source of truth; writing them moves
    // the clock and the change sticks and keeps flowing (live-verified).
    //
    // Struct layout (from the h/m/s reconstruction math in sub_871360, and
    // live-confirmed 2026-07-20: read day=42 hour=13 min=46 sec=39 matched the
    // in-game clock exactly):
    //   +0x00 i32 day    +0x04 i32 hour    +0x08 i32 minute    +0x0C i32 second
    //
    // Located by a unique signature over the realm-select read: a TLS realm
    // probe followed by the two `vmovups ymm0, cs:<global>` (server if the
    // probed byte is set, else client). The two RIP operands resolve to the
    // server and client globals.
    //
    // The TLS index here is NOT kTls_RealmFlag. There are two selector bytes
    // and 2.01.00 moved both: the general one 498 -> 509 (used by ~11,400
    // sites), and this clock-specific one 502 -> 492 (used by ~150). They are
    // genuinely different slots, so do not reconcile them - the constant is
    // baked into the pattern below and must stay exact.
    //
    // Verified in 2760: unique at 0x14202EF24, resolving the server clock to
    // 0x146931D68 and the client clock to 0x146931D48 - 0x20 apart, both in a
    // data section, which is the shape a pair of 32-byte clock structs should
    // have. The relative layout inside the match is unchanged.
    inline constexpr const char* kSig_FieldTimeRealm =
        "BA EC 01 00 00 48 8B 08 0F B6 04 0A 84 C0 74 0A C5 FC 10 05 "
        "?? ?? ?? ?? EB 08 C5 FC 10 05 ?? ?? ?? ??";
    // Within the match: server `vmovups` at +0x10, client `vmovups` at +0x1A;
    // each is 8 bytes (4-byte opcode C5 FC 10 05 + 4-byte disp at its tail).
    inline constexpr uintptr_t kOff_FieldTime_ServerVmovups = 0x10;
    inline constexpr uintptr_t kOff_FieldTime_ClientVmovups = 0x1A;
    inline constexpr int       kLen_FieldTime_Vmovups       = 8;
    // Time struct fields (int32 each).
    inline constexpr uintptr_t kOff_FieldTime_Day  = 0x00;
    inline constexpr uintptr_t kOff_FieldTime_Hour = 0x04;
    inline constexpr uintptr_t kOff_FieldTime_Min  = 0x08;
    inline constexpr uintptr_t kOff_FieldTime_Sec  = 0x0C;

    // --- Time of Day: FREEZE via the field-time tick -------------------------
    // sub_871360 is the per-frame FieldTime tick. Its very first act is
    //   [mgr+0x2C] += frameDeltaSeconds   (a float accumulator)
    // and EVERYTHING downstream advances from it: the reconstruction that
    // rewrites the two realm clock globals above (every game tick, via
    // sub_8719B0) and the sun/sky sync. So freezing the clock is simply forcing
    // that delta to 0 while frozen - the accumulator holds, the globals stop
    // being rewritten, the sun stops, and NOTHING else is touched (physics, AI
    // and combat advance on their own deltas).
    //
    // This supersedes the old pin-the-globals-every-frame freeze, which lost a
    // race against this very function: it rewrites the globals every game tick
    // from its accumulator, so a per-frame pin from another thread never held.
    //
    // The tick is __fastcall(rcx=mgr, xmm1=delta, xmm2=unused); the delta is a
    // single float in xmm1, so the detour prototype declares `float delta` to
    // land on xmm1 and zeroes it. Signature = the ABI-fixed prologue plus the
    // distinctive accumulator add `vaddss xmm0, xmm1, [rcx+2Ch]`
    // (make_signature_for_function, unique in this build).
    inline constexpr const char* kSig_FieldTimeTick =
        "48 89 5C 24 ?? 48 89 74 24 ?? 48 89 7C 24 ?? 4C 89 64 24 ?? "
        "55 41 56 41 57 48 8B EC 48 83 EC 70 48 8B F9 C5 F2 58 41 2C";

    // --- Time of Day: FREEZE the visible SUN via the RENDER manager ----------
    // The numeric field clock above is only half the story. The visible
    // day/night (sun/moon/sky) is driven by the RENDER "TimeOfDay" manager's
    // currentTimeOfDay float, which advances on its OWN per-frame accumulator -
    // so freezing the field-time tick stops the numeric clock but the SUN
    // keeps moving (the long-standing "Freeze only freezes the clock" bug).
    //
    // The engine's own debug commands PROVE this is the right layer:
    // PearlAbyssEngine.Debug.TimeOfDayForward / Backward / *x2 (handlers
    // sub_2F616F0 / sub_2F61690 / sub_2F617B0 / sub_2F61750) all call the
    // manager's AdvanceTime (vtable +0x130) to move the sun, and the engine
    // console's /settimeofdaylowerlimit + /settimeofdayupperlimit (handlers
    // sub_31FB810 / sub_31FB860) clamp currentTimeOfDay into [lower,upper].
    // So the reliable sun freeze = force lower == upper == the captured hour
    // every tick; the engine pins the sun to it while real time keeps flowing.
    // Restored to the originals on disable/unload.
    //
    // Runtime chain (current build, re-RE'd 2026-07-21):
    //   engine  = *qword_648F688
    //   manager = *(engine + 0x2F8)   (engine vtable slot 8 getter sub_36341F0
    //                                  = `return *(engine + 0x2F8)`)
    //   manager+0x3D0 f32 currentTimeOfDay (hours 0..24)
    //   manager+0x3D4 f32 lowerLimit
    //   manager+0x3D8 f32 upperLimit
    //
    // qword_648F688 is BSS (runtime-populated by the engine-console registrar
    // sub_31FAB10). Anchored on its one-time init store, guarded by the
    // dword_648F680 == -1 check that precedes it:
    //   cmp cs:dword_648F680, -1 ; jnz ; mov cs:qword_648F688, rbx ; mov cs:.., rdi
    // The engine global = RIP target of that first `mov cs:<g>, rbx` store.
    inline constexpr const char* kSig_TodEngineGlobal =
        "83 3D ?? ?? ?? ?? FF 75 ?? 48 89 1D ?? ?? ?? ?? 48 89 3D";
    inline constexpr uintptr_t kOff_TodEngineGlobal_Mov = 9; // the `48 89 1D <disp32>`
    inline constexpr int       kLen_TodEngineGlobal_Mov = 7; // 3-byte opcode + disp32
    inline constexpr uintptr_t kOff_Tod_Manager     = 0x2F8; // engine -> render manager
    inline constexpr uintptr_t kOff_Tod_CurrentHour = 0x3D0; // f32 hours 0..24
    inline constexpr uintptr_t kOff_Tod_LowerLimit  = 0x3D4; // f32 clamp lower
    inline constexpr uintptr_t kOff_Tod_UpperLimit  = 0x3D8; // f32 clamp upper

    // --- Armor dye / material / repair-condition (dye.cpp) -------------------
    // The dyehouse system, fully RE'd 2026-07-17 from the server's own dye
    // transaction (IDB sub_257C330 - it logs "sql->dyeItem"):
    //
    // An item's dye state is a vector of 16-byte "dye records" ON THE ITEM
    // VALUE ITSELF - the same 192-byte TrItemValue the inventory code already
    // edits (which is why dye survives unequip/re-equip and saves: it is item
    // state, persisted to the save DB by that transaction). Record layout
    // (field roles derived from this build's own record copier, IDB
    // sub_D20110):
    //     +0  u32  color-group key (dyecolorgroupinfo._key; the dyehouse UI's
    //              color family). The renderer reads the RGB verbatim - this
    //              key just records WHICH palette the color came from - but
    //              write a real one so the game's own dye UI stays coherent.
    //     +4  u16  material template 1..10 into partprefabdyetexturepalleteinfo
    //              (cloth/leather/metal texture variants); 0xFFFF = the item's
    //              natural material.
    //     +6  u8   channel ("mod" 0..11) - WHICH colorable zone of the mesh.
    //              An item defines up to 12 (partprefabdyeslotinfo); records
    //              are keyed by this byte, one per channel.
    //     +7  u8 r, +8 u8 g, +9 u8 b, +10 0xFF
    //     +11 u8   repair condition: 0 = pristine .. 0x7F = fully weathered
    //              (0xFF = legacy "no override", renders pristine)
    //     +13 u8   0x04 on channels 0 and 3 in natural records (mirrored for
    //              shape fidelity; the engine accepts records without it)
    //
    // The vector lives at itemVal+0x70 (data) / +0x78 (u32 count), capped at
    // 12 records; the engine upserts by channel byte. The same TrItemValue
    // shape (with a u16 slot tag appended at +0xC0, stride 0xC8) is what the
    // equip component keeps per equipped slot in its table at comp+0x88 -
    // walking that table IS "what am I wearing right now".
    //
    // HOW WE APPLY: the client's own dye-ack handler (IDB sub_7D9C50) - what
    // runs when the dyehouse server transaction acks (message 2440) - takes
    // (equipComponent, int* err, batchBlob) and does everything: upserts the
    // records into the equipped entry, live-updates the rendered materials
    // per channel (sub_7DA640 set / sub_7DB870 clear - no mesh teardown, no
    // re-equip), propagates to linked parts, and kicks the HUD refresh. We
    // build the blob and call it - the game's own code path end to end.
    // Server-side validation of the record is NONE (the wire handler
    // sub_24588D0 passes arbitrary RGB through), so free-color dyeing works.
    //
    // The batch blob is 10 blocks x 196 bytes: u16 slotTag (0xFFFF = block
    // unused), u16 pad, then 12 x 16-byte records. Records whose channel byte
    // has the high bit set (we use 0xFF) are skipped; an all-zero record with
    // material 0xFFFF and repair 0xFF means CLEAR that channel.
    //
    // PERSISTENCE: worn equipment is NOT in any inventory holder, so there is
    // no inventory item to mirror dye onto - the equip table IS the item's
    // home while it is worn. Proven by the equip path (IDB sub_7D7470): when a
    // slot is vacated it PUTS THE OLD ITEM BACK into a bucket
    // (sub_1CE2600 -> sub_F4B8F50, "copy item value into bucket slot, or add
    // to the stack there"), which would duplicate the item if equipping had
    // left it in the bag. The entry carries the item whole - the item-value
    // copy (sub_F4DAD00) copies the dye vector (+0x70/+0x78) with it, which is
    // also why dye survives an unequip.
    //
    // So the durable target is the SERVER realm's equip component, not a
    // holder. sub_7D9C50 renders, and we only ever call it on the client's
    // component; the server's entry is written as plain data with the engine's
    // own upsert primitive (sub_1F8CB40: find record by channel, overwrite,
    // else append via sub_D20110 if count < 12) - no render calls on a server
    // actor. The append can ALLOCATE, so that write runs with the realm flag
    // flipped (kTls_RealmFlag), exactly like the add-item path.
    //
    // (An earlier design searched both holders by instance id and always came
    // up empty - live-confirmed 2026-07-17, every slot: "no client-side item
    // with instance id 1001675". The visual apply was fine; the item was never
    // there to find.)
    //
    // The dyehouse's own server transaction (sub_257C330) addresses items by
    // (storageType, slotIndex) via sub_1CE3CB0 - i.e. it dyes an item sitting
    // in a bucket, then broadcasts the result, which is the ack we call
    // directly. That path is no use to us for worn gear, for the reason above.

    // The equip component of a character: *(*(actor + 0x68) + 0x38). The
    // engine's own route - the dye-ack dispatcher (IDB sub_9BDF30) resolves
    // the broadcast's actor and calls the applier with
    // `*(*(actor + 104) + 56)`, and BatchEquip reaches the same object from
    // the other side (`*(*(comp+8) + 104) + 56`). comp+0x08 is the owning
    // actor, which makes the walk self-validating: resolve the component from
    // an actor, then require it to point back.
    //
    // This is what lets us reach BOTH realms without waiting on a hook: the
    // character actor IS what inventory.cpp calls a container (its holder walk
    // *(*(actor+0x68)+0xB8) is sub_1CDE460 verbatim), and that file already
    // resolves the client's by global walk and the server's from the commit
    // hook's capture list. A hooked component is whichever realm happened to
    // fire, and only re-fires when the player changes gear.
    inline constexpr uintptr_t kOff_Sub_EquipComp = 0x38; // (actor+0x68)+0x38
    inline constexpr uintptr_t kOff_EquipComp_Owner = 0x08; // -> the actor

    // The equip-batch function (IDB sub_7C98D0, "BatchEquip"): its rcx is the
    // component whose +0x88 table drives everything above.
    // Hooked as a FALLBACK capture path for the walk above (and as the signal
    // that a gear change happened). Fires on every player equip change
    // including the initial load-in dress-up. Signature = full
    // prologue through the arg shuffle (mov r15,r8; mov r12,rdx; mov r14,rcx;
    // mov r13,[rcx+8]); stack/frame immediates wildcarded. Unique.
    inline constexpr const char* kSig_EquipBatch =
        "48 89 5C 24 ?? 55 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 "
        "?? ?? ?? ?? B8 ?? ?? ?? ?? E8 ?? ?? ?? ?? 48 2B E0 4D 8B F8 "
        "4C 8B E2 48 8B F1 4C 8B 71 08";

    // The client dye-ack applier (IDB sub_7D9C50):
    //     int* f(void* equipComponent, int* outErr, void* batch1960)
    // Called directly with our crafted batch. Signature = prologue + the
    // literal 0x120 frame + arg shuffle (mov r12,r8; mov rsi,rdx). If a patch
    // resizes the frame, re-find via xrefs to the dye upsert (kSig_DyeUpsert)
    // from a ~0x550-byte function in the equip-component code region.
    // 1.17.00 recompiled this with a much smaller frame - 0x120 down to 0x50 -
    // and dropped the `lea rbp` entirely, which is why nothing matched even
    // with the frame wildcarded. What did survive is the part the ABI fixes:
    // the callee-saved push sequence and the three-argument shuffle
    //     mov r12, r8    ; batch
    //     mov rsi, rdx   ; outErr
    //     mov r14, rcx   ; equip component
    // which is exactly the prototype. Found by neighbourhood: it sits +0x10050
    // from BatchEquip here, against +0x10380 in the analysed build, and it
    // walks the same equip table. Unique.
    inline constexpr const char* kSig_DyeApplyBatch =
        "4C 89 44 24 ?? 48 89 54 24 ?? 55 53 56 57 41 54 41 55 41 56 "
        "41 57 48 8B EC 48 83 EC 68 49 8B F0";

    // The dye-record upsert primitive (IDB sub_1F8CB40):
    //     void f(void* itemVal, const uint8_t record[16])
    // Finds the record whose +6 channel matches, overwrites it; else appends
    // (growing the vector in the CALLING THREAD'S REALM) while count < 12.
    // Used for the inventory-instance mirror. The `49 C1 E0 04` is the
    // 16-byte record stride (shl r8,4) - semantic, keep literal.
    // Re-anchored for 2850 on the function's OWN body rather than a prologue.
    // The previous pattern matched 0x142355580 - a four-argument diff batcher
    // that walks the same vector read-only and returns without touching it, so
    // Trinity called it, got no fault, and reported twelve channels "newly
    // created" while the count at +0x80 stayed zero. The save then wrote what
    // was there: nothing. That is worth stating plainly, because the header
    // comment above had the arity right all along and the prototype in dye.cpp
    // drifted away from it.
    //
    // This pattern is the upsert's first six instructions, and every one of
    // them is load-bearing rather than incidental:
    //     48 8B 41 78     mov  rax, [rcx+0x78]   ; kOff_ItemVal_DyeData
    //     4C 8D 49 78     lea  r9,  [rcx+0x78]
    //     45 8B 51 08     mov  r10d,[r9+8]       ; count, i.e. itemVal+0x80
    //     41 8B CA        mov  ecx, r10d
    //     48 C1 E1 04     shl  rcx, 4            ; the 16-byte record stride
    //     48 03 C8        add  rcx, rax
    //     48 3B C1        cmp  rax, rcx
    // Unique in 2850, at 0x142355870.
    inline constexpr const char* kSig_DyeUpsert =
        "48 8B 41 78 4C 8D 49 78 45 8B 51 08 41 8B CA 48 C1 E1 04 48 "
        "03 C8 48 3B C1";

    // Equip component layout (verified in THIS build from BatchEquip's own
    // table walk: `a1[17]` -> desc, `*(desc+8) + 200*i`, tag at +192).
    // 1.17.00 moved the descriptor DOWN one qword, 0x88 -> 0x80. BatchEquip
    // reads it twice and never touches +0x88 any more. Note the direction: this
    // is the one field in this build that moved down rather than up.
    inline constexpr uintptr_t kOff_EquipComp_Table  = 0x90; // -> table descriptor (was 0x80 before 2.01.00)
    inline constexpr uintptr_t kOff_EquipTable_Array = 0x08; // entry[] base
    inline constexpr uintptr_t kOff_EquipTable_Count = 0x10; // u32
    // 0xC8 -> 0xD0, following TrItemValue growing 0xC0 -> 0xC8: an entry is
    // still a value plus a u16 tag, so it grew with it.
    inline constexpr uintptr_t kEquipEntry_Stride    = 0xD0; // TrItemValue + u16 tag
    // Sits immediately after the value, so it moved with it: 0xC0 -> 0xC8.
    inline constexpr uintptr_t kOff_EquipEntry_SlotTag = 0xC8; // u16 (helm 3, chest 4,
                                                               // gloves 5, boots 6, cloak 16)
    // Within an entry, the TrItemValue fields reuse kOff_ItemVal_InstanceId /
    // kOff_InvSlot_TypeId / kOff_InvSlot_Quantity above, plus:
    // +8 with TrItemValue (0xC0 -> 0xC8). The ctor zeroes the pair at +0x78 /
    // +0x80, which is this vector. Reading it at +0x70 gave a wrong channel
    // mask, so dyeing behaved as one colour for the whole item instead of the
    // per-part channels the game exposes.
    inline constexpr uintptr_t kOff_ItemVal_DyeData  = 0x78; // 16-byte record[]
    inline constexpr uintptr_t kOff_ItemVal_DyeCount = 0x80; // u32
    inline constexpr uint32_t  kDye_MaxChannels      = 12;
    // The dye record's own shape. These lived as bare numbers inside dye.cpp -
    // `data + i * 16 + 6` and friends - which is precisely why a 153-constant
    // audit of this header could sweep the whole build and still miss them. A
    // struct offset that is not named here is a struct offset nobody checks
    // after a patch, and the move-owner offset that hid in teleport.cpp through
    // all of 2.01.00 is the same lesson learned twice.
    //
    // Read straight off the upsert's copy block at 0x1423558AE..0x1423558EB,
    // which moves rec[0x00..0x0C] into the record one field at a time, and off
    // its `shl rcx, 4` stride at 0x14235587F. Unchanged since 2.01.00.
    inline constexpr uintptr_t kDyeRec_Stride        = 16;
    inline constexpr uintptr_t kOff_DyeRec_Channel   = 0x06; // u8
    inline constexpr uintptr_t kOff_DyeRec_R         = 0x07; // u8
    inline constexpr uintptr_t kOff_DyeRec_G         = 0x08; // u8
    inline constexpr uintptr_t kOff_DyeRec_B         = 0x09; // u8

    // --- Abyss Gear sockets (live-cracked 2026-07-18; see the abyss-gear note) -
    // Every worn item's TrItemValue carries a socket list right next to its dye
    // vector: a PRE-ALLOCATED 5-slot vector of 6-byte records, plus a separate
    // inline count of how many sockets are actually unlocked. The record array
    // is index-addressed (record[i] = socket i); the unlocked count is the real
    // "how many sockets" driver, not the record's own index byte. Both fields
    // ride with the item value on save (sub_F4DAD00 deep-copies the +0x58 vector
    // and the +0x68 count), so a server-realm write persists exactly like dye.
    //
    // Add/remove a gear touches ONLY a record's bytes (that is all the game's own
    // Witch-socket does); unlocking a NEW socket also grows a save-data sublist
    // inside the +0x58 target at +0xC0, which we do NOT reproduce - so unlocking
    // renders live but is not durable yet (see Equipment::UnlockAll).
    // +8 with TrItemValue. +0x58 is a plain u16 in this build - the ctor
    // writes `mov word ptr [rcx+0x58], bx` - and the socket vector now starts
    // at +0x60, which the ctor takes as a sub-object base with `lea rax,
    // [rcx+0x60]`. Reading records from +0x58 produced garbage gear ids, which
    // is why every socket displayed the same wrong item.
    inline constexpr uintptr_t kOff_ItemVal_SocketData     = 0x60; // -> record[] (target also has a save sublist @+0xC8)
    inline constexpr uintptr_t kOff_ItemVal_SocketSize     = 0x68; // u32 vector size (always 5)
    inline constexpr uintptr_t kOff_ItemVal_SocketCap      = 0x6C; // u32 vector capacity (5)
    inline constexpr uintptr_t kOff_ItemVal_SocketUnlocked = 0x70; // u32 unlocked-socket count = the real socket count
    inline constexpr uintptr_t kSocketRec_Stride           = 6;
    inline constexpr int       kSocket_Max                 = 5;    // absolute max (matches the vector capacity)
    // Record layout (6 bytes):
    inline constexpr uintptr_t kOff_SockRec_GearId = 0; // u16 abyss-gear typeId (0xFFFF = empty)
    inline constexpr uintptr_t kOff_SockRec_Marker = 2; // u16 0xFFFF when filled, 0x0000 when empty
    inline constexpr uintptr_t kOff_SockRec_Index  = 4; // u8  socket index (0xFF = the game's "locked" record)
    inline constexpr uintptr_t kOff_SockRec_State  = 5; // u8  0x05 filled, 0x00 empty
    inline constexpr uint16_t  kSock_Empty         = 0xFFFF;

    // --- Refinement level (the equipment "refinement"/enhancement upgrade) -----
    // Every piece refines up to level 10 (wiki: "Refinement"; internally the
    // enhancement/"강화" concept). The level lives inline on the TrItemValue at
    // +0x0A - the SAME u16 offsets.h otherwise calls the item subtype: a freshly
    // made stack reads 0 there, a refined piece carries its level (seen 7->8 live
    // in the value copier sub_F4DAD00). Because sub_F4DAD00 copies +0x0A, the
    // level rides with the item value on save, so - exactly like dye/sockets - a
    // write mirrored into BOTH realms persists (a client-only write is undone by
    // the server->client reconcile, which zeroes a client +0x0A). Two things stay
    // live-verify-only, so treat them as unconfirmed until tested in-game:
    //   * whether bumping +0x0A alone re-derives the piece's stats (real
    //     refinement also rebuilds derived data and spins a new item instance);
    //     we trigger the same effect refresh a socket edit does as the best lever.
    //   * whether the server accepts an out-of-band level on reconcile/save.
    inline constexpr uintptr_t kOff_ItemVal_RefineLevel = 0x0A; // u16, == kOff_ItemVal_Subtype
    inline constexpr int       kRefine_Max              = 10;

    // Which levels a given item can actually be refined to. NOT every item is
    // refinable, and the ones that are do not all stop at 10 - the engine keeps
    // a per-item list and matches +0x0A against it entry by entry.
    //
    // RE 2026-08-29 (static, verified against the shipped TU 2.00.00 exe):
    //   * ItemInfo+0x248 is _enchantDataList - a plain vector: data pointer at
    //     +0x248, element count (u32) at +0x250, capacity at +0x254. The
    //     deserializer at 0x141308B00 grows it and writes each element with
    //     `imul rcx, rax, 0x70`, so the stride is 0x70.
    //   * Each element is an EnchantData, and its FIRST field is _level, read
    //     two bytes wide at element+0x00 by 0x1412E9EC0 (the failure string
    //     "EnchantData? _level..." sits on that read's error path). +0x08 is
    //     _enchantStatData, +0x48 _buyPriceList - i.e. the stats the level
    //     grants hang off the same record.
    //   * ItemInfo+0x218 is _dropDefaultData, whose own first u16 is
    //     _dropEnchantLevel - the value the item CONSTRUCTOR seeds +0x0A from.
    //     That is the direct proof +0x0A is the enchant level and not a general
    //     subtype discriminator, despite kOff_ItemVal_Subtype naming it that.
    //
    // Why this matters: writing 10 into +0x0A on an item whose list has no such
    // entry (or no list at all - fists, cosmetics, tools) leaves the engine
    // holding a level it cannot resolve to any EnchantData, which is the shape
    // of the "max refine, then switch to hands" crash. Gate every write on this.
    inline constexpr uintptr_t kOff_ItemDef_EnchantList      = 0x248; // ptr, ItemInfo row
    inline constexpr uintptr_t kOff_ItemDef_EnchantCount     = 0x250; // u32
    inline constexpr uintptr_t kOff_EnchantData_Stride       = 0x70;
    inline constexpr uintptr_t kOff_EnchantData_Level        = 0x00; // u16
    inline constexpr uint32_t  kEnchantList_SaneMax          = 64;   // reject junk counts

    // The equipped-item EFFECT refresh (IDB sub_7C88A0): re-applies every
    // equipped item's effects - re-reading each item's abyss-gear sockets and
    // REBUILDING its derived effect data (sub_7C55B0 per item), then the final
    // recompute pass (sub_7E1160 -> sub_7CB670/sub_7CCBD0). This is exactly what
    // the Witch's own socket action runs. A raw socket write updates the record
    // but leaves the derived effect structure stale, so the gear stays dormant
    // until a reload; calling this on the client equip component (game thread)
    // makes it take hold live. (An earlier attempt called only the last-stage
    // re-aggregators sub_7CB670/sub_7CCBD0 - they READ the derived structure and
    // do not rebuild it, so they did nothing on a raw write. Live trace of the
    // Witch's socketing found sub_7C88A0 as the real entry.)
    // Signature: void* f(equipComponent, int* out).
    inline constexpr const char* kSig_EquipEffectRefresh =
        "48 89 5C 24 20 55 56 57 48 8D 6C 24 B9 48 81 EC 90 00 00 00 "
        "48 8B D9 80 B9 F8 00 00 00 00";

    // --- Why live REMOVAL of an abyss gear does not strip its effect -----------
    // (RE 2026-07-19, static trace of the whole effect cluster around sub_7C88A0)
    //
    // The applied stat/skill effects do NOT live in any list we can just clear.
    // The engine applies them through a DIFF ENGINE: sub_7CA0E0(comp, err,
    // &newList, flag, &oldList) -> sub_7CFBD0(comp, &newList, &oldList) computes
    // new-minus-old and pushes the delta into the character stat aggregator. An
    // effect is only REMOVED when the gone gear is present in the caller's OLD
    // list. Every apply path (sub_7C98D0 batch equip, sub_7D7470 single equip,
    // sub_7DE320 per-slot) funnels through sub_7CA0E0.
    //
    // Two dead ends already tried and disproven live:
    //   * sub_7CB670 / sub_7CCBD0 - READ the derived data, do not rebuild it.
    //   * the append-only accumulator acc = *(*(actor+0x68)+0x178) (list
    //     {data@+0x118, count@+0x120, cap@+0x124}, sub_7C55B0 appends to it) -
    //     clearing its count before sub_7C88A0 did NOT strip the effect, because
    //     the effect was already pushed downstream by the diff apply; the list is
    //     just a working ledger, not the applied state.
    //
    // The caller always re-derives its "old" list from the item's CURRENT socket
    // records, so once we have already emptied the record a raw refresh (or even
    // the game's own unequip/re-equip) can never see the removed gear in "old" ->
    // nothing to subtract. Only a full reload fixes it, because the item's whole
    // effect state is rebuilt from scratch on load. A correct live fix must drive
    // sub_7CA0E0 with an OLD list that still contains the removed gear (or call
    // the game's own Witch unsocket handler) - needs a live trace to pin, do NOT
    // ship another blind clear.

    // Batch blob geometry for kSig_DyeApplyBatch.
    inline constexpr size_t    kDyeBatch_Blocks     = 10;
    inline constexpr size_t    kDyeBatch_BlockSize  = 196;  // u16 tag + pad + 12*16
    inline constexpr size_t    kDyeBatch_RecordsOff = 4;
    inline constexpr size_t    kDyeBatch_Size       = kDyeBatch_Blocks * kDyeBatch_BlockSize;

    // The color families (dyecolorgroupinfo keys + preset shades) and the
    // dyeable-prefab registry (partprefabdyeslotinfo) live in dye_data.h,
    // generated straight from this install's own data tables by
    // scripts/gen_dye_data.py - rerun it after a game patch.

    // --- Trust Multiplier: scale the friendly ("Friendly"/친밀도) gain --------
    // "Friendly" is the engine's TRUST value for both NPCs (gifting) and
    // animals/mounts (feeding-to-tame): strings UI_Gift_Friendly_IncreaseAmount
    // and UI_Vehicle_FriendlyLevelUp, runtime field _varyFriendly. It is a
    // 0..100 value; reaching 100 is what completes taming.
    //
    // The value is stored in a per-relationship 0x58-byte RECORD held in the
    // friendly component, and EVERY write to it - gift, feed, AI, save-load,
    // and the network sync - funnels through ONE of two leaf setters (both
    // live-confirmed: a HW watchpoint on an NPC's trust value broke at the
    // update store 0xDBE114F inside the NPC setter). The batch/RPC path
    // (sub_613220) does NOT fire on a direct gift - it is downstream of these,
    // so hooking the setters is strictly more complete:
    //   _DWORD* setNpc(void* npcMap /*=comp+0x18 owner*/, void* record)  // IDB sub_DBE1000
    //   _DWORD* setPet(void* petMap /*=comp+0x38 owner*/, void* record)  // IDB sub_1AD4710
    // Both take the destination map owner in rcx and the SOURCE record in rdx;
    // they locate/insert the matching slot and copy the 0x58-byte record in.
    // Record layout (the copy is 4 SIMD stores at +0x00/+0x20/+0x40/+0x50):
    //   record +0x00 : u32  record key   (matched within the bucket)
    //   record +0x04 : u16  group key    (selects the bucket)
    //   record +0x20 : i64  TRUST value  (the field written at 0xDBE114F; the
    //                       same field sub_613220 compares >= 100 to tame)
    // The two setters differ only by which map they target (NPC comp+0x18 vs
    // pet/vehicle comp+0x38), which is the byte the prologue's `lea rbp,[rcx+..]`
    // encodes - that displacement is the signature discriminator.
    //
    // The Trust Multiplier scales the GAIN: for each write, compare the record's
    // new value to the last value we let through for that (map,group,key) and,
    // if it went up, rewrite value = clamp(old + (new-old)*mult, 0, 100). The
    // cache is seeded (unscaled) the first time a key is seen - and because the
    // save-loader drives these SAME setters at login, every relationship is
    // pre-seeded there, so the first in-game gift/feed is already scaled and a
    // loaded save is never re-scaled. See the trinity-friendly-system notes.
    //
    // TU 2.00.01 (1.0.0.2658) reshaped this one twice over, which is why the
    // two patterns below both went dead and NPC Trust Multiplier disabled
    // itself with nothing else in the log:
    //
    //  * The map offset is BAKED again. 2.00.00 computed it at runtime
    //    (mov eax,imm32 / add eax,[global] / lea rbp,[rcx+rax]); 2.00.01 went
    //    back to a plain `lea rbp,[rcx+0x18]`. That +0x18 is load-bearing and
    //    must never be wildcarded - it is the whole difference between this
    //    function and the pet setter below, which takes +0x38. Both maps hang
    //    off the same owner and the two setters are genuinely separate: at
    //    0x14239BD31 a single `jne` picks `call 0x141BDC2D0` (pet) or
    //    `call 0x141BDBF60` (NPC), so one hook could never have covered both.
    //
    //  * The BODY MOVED OUT of the main code section. 0x141BDBF60 - where the
    //    function used to live, right after the +0x18 query - now holds nothing
    //    but a 5-byte `E9` jump to 0x14D6E97D0, over in .sbss (the packer's
    //    unpacked-code region; kSig_StatCommit already resolves there too, so
    //    the scanner handles it). This pattern deliberately describes the real
    //    body, not the trampoline: MinHook needs five bytes to patch and the
    //    trampoline is exactly five bytes long.
    //
    // TU 2.01.00 (build 2760) broke all three NPC patterns and the pet one at
    // once, and this time nothing semantic changed at all: the `lea rbp,[rcx+
    // 0x18]` that carried the map offset is simply gone, folded into how the
    // recompiled body indexes the map. The prologue is otherwise the same
    // function it has always been.
    //
    // So both signatures now anchor INSIDE the body, on the map access itself,
    // and are installed with InstallHookInterior (the function start comes from
    // the unwind tables). The two setters remain byte-for-byte the same code
    // compiled against different base offsets, and the map offsets are still
    // the entire difference between them:
    //
    //     NPC  count [rsi+0x18]  buckets [rsi+0x28]  values [rsi+0x30]
    //     pet  count [rsi+0x38]  buckets [rsi+0x48]  values [rsi+0x50]
    //
    // Those offsets must never be wildcarded - wildcard them and the two
    // patterns collapse into each other, and the pet hook lands on NPCs.
    //
    // Both still end in the same record-replacement copy: find the record whose
    // key at +0x00 matches, then blit 0x68 bytes over it (three vmovups plus a
    // vmovsd tail), which is how the trust value at record+0x20 gets written.
    // That confirms these are the setters and not some neighbouring lookup.
    //
    // The bodies still live outside the main code section, reached through a
    // 5-byte `E9` trampoline - the packer's region, `.debug$P` in this build
    // (it renames its sections every patch, so nothing may key on the name).
    // These patterns describe the real body, not the trampoline: MinHook needs
    // five bytes to patch and the trampoline is exactly five bytes long.
    //
    // Verified in 2760: one match each, NPC at 0x14D882B40, pet at 0x141E2AD40.
    // The older prologue-based patterns are dropped rather than kept as
    // fallbacks: they described a `lea` that no longer exists, so they could
    // only ever fail, and carrying dead patterns makes a log say "tried 3
    // variants" when it really tried one idea three times.
    inline constexpr const char* kSig_FriendlySetNpc =
        "39 6E 1C 74 ?? 44 8B 00 8B 4E 18 85 C9";
    inline constexpr const char* kSig_FriendlySetPet =
        "39 6E 3C 74 ?? 44 8B 00 8B 4E 38 85 C9";

    // The OTHER trust write path, and the one that actually carries greet,
    // dialogue rewards, petting, feeding and wild taming. The two setters
    // above are a record REPLACEMENT pipeline: they take a fully-built
    // 0x58-byte record and copy it into the map, which is what a gift and
    // the save/network sync do. An incremental gain never builds a record -
    // it hands a raw delta to this accumulator instead, so it was invisible
    // to the hooks above and greet/feed multiplied nothing. That is the
    // "Trust Multiplier is not working" report; gift always worked.
    //
    // Which path an interaction takes is decided at 0x14239BC73:
    //     cmp byte ptr [rdi+0x8A], 0
    //     jne 0x14239BD1B          ; set -> full record -> the setters (GIFT)
    //     ...
    //     mov r9, qword ptr [rdi+0x20]   ; clear -> raw delta gain
    //     call 0x141BDDB00               ; -> this function (GREET / FEED)
    //
    // __fastcall(void* rel, uint32_t* status, uint16_t group, int64_t delta):
    //   rcx  relationship object - u32 tier at +0x00, i64 trust at +0x08
    //   rdx  status/result out-pointer (also the return value)
    //   r8w  group id
    //   r9   the delta gain, and the only thing a multiplier needs to touch
    //
    // r9 being the gain is not inferred - the function early-outs on it:
    //     0x14D6F3376  test r9, r9
    //     0x14D6F3379  jg   0x14D6F3388    ; <=0 returns a status, writes nothing
    // and it clamps to 100 and advances the tier itself, so scaling the delta
    // needs no cache, no baseline and no clamp of our own.
    //
    // Body lives in .sbss behind a 5-byte E9 trampoline at 0x141BDDB00, same
    // shape as kSig_FriendlySetNpc_20001. All FOUR call sites in 2658 go
    // through that trampoline and none reaches the body directly, so hooking
    // the body covers every one: 0x14239BCE0 and 0x14239BEC8 (NPC greet and
    // dialogue), 0x14250D58E (mount), 0x1405244A9 (wild animal feed/tame).
    // Frame size and the rbp lea wildcarded; the register moves that define
    // the ABI are kept. Verified: exactly one match in 2658, at 0x14D6F3350.
    // The MOUNT call site of kSig_FriendlyAddDelta, so the multiplier can leave
    // it alone. Riding gains +1 per tick through this site; scaling it in any
    // shape kills the game - measured over three separate attempts (inflate the
    // delta, clamp the resulting total, repeat the call N times), all three
    // crashed identically at CrimsonDesert.exe+0x374E2E6 on a tagged pointer
    // inside the mount class. The mount's bond is simply not ours to accelerate.
    //
    // Matched rather than baked: the four call sites are documented below as
    // literal addresses, but a literal is exactly what went stale as
    // kMountVtableOffset_TU20000 (see player.cpp). This is the call setup
    // itself - `lea rax,[rbp+X]` / spill / `mov r9,r13` / `movzx r8d,[rsi+0x30]`
    // / `lea rdx,[rbp+Y]` - with only the two frame displacements wildcarded.
    // Verified: exactly one match in 2658, at 0x14250D579, whose E8 sits at
    // +0x15 and therefore returns to +0x1A.
    // 2760 changed exactly one BIT of this: `movzx r8d,[rsi+0x30]` became
    // `movzx r8d,[r14+0x30]`, so the REX byte went 44 -> 45. Everything else,
    // including the argument shuffle and the 0x30 struct offset, is identical
    // to 2658. That one bit is also what identifies the site as the mount's:
    // of the accumulator's four call sites, only this one still matches the
    // shape the old comment described. Unique (1) at 0x142880E62, inside the
    // mount function 0x142880DC0.
    inline constexpr const char* kSig_FriendlyMountGainCall =
        "48 8D 45 ?? 48 89 44 24 20 4D 8B CD 45 0F B7 46 30 "
        "48 8D 55 ?? E8";
    inline constexpr uintptr_t   kOff_MountGainCall_Ret = 0x1A;

    inline constexpr const char* kSig_FriendlyAddDelta =
        "66 44 89 44 24 18 55 53 57 41 56 41 57 48 8D 6C 24 ? 48 81 EC ? ? ? ? "
        "4C 89 CF 41 0F B7 D8 49 89 D6 49 89 CF 4D 85 C9";
    // The NPC interaction dispatcher - the caller that hands greet, dialogue
    // and gift their trust change. Hooking THIS is the way back to scaling
    // greet without touching kSig_FriendlyAddDelta, which cannot be hooked:
    //
    //   the accumulator 0x14D6F3350 has SPLIT unwind data - a 60-byte
    //   RUNTIME_FUNCTION plus two more entries starting inside its own body -
    //   so patching its prologue leaves the unwinder replaying pushes that are
    //   no longer there. That is why a detour doing NOTHING still killed the
    //   game at +0x239BEF9, which is itself inside the second NPC interaction
    //   function, on the way back out. Every function Trinity hooks safely
    //   (the trust setters, the master frame update) has ONE unwind entry
    //   spanning its whole body. This dispatcher does too: a single
    //   0x14239BC40..0x14239BD65, 293 bytes, nothing starting inside it.
    //
    // It also keeps the mount away from us. The accumulator has four call
    // sites and they live in four different functions - NPC greet/dialogue
    // here, a second NPC site in 0x14239BD70, wild animals in 0x1405243F0,
    // and the MOUNT in 0x14250D4E0. Scaling a mount gain is separately fatal
    // (+0x374E2E6, inside the mount class), so staying out of that function
    // matters. This one has two callers and neither is the mount function,
    // and the mount function does not reach it within two call levels - but
    // that is a bounded static search past which virtual dispatch is opaque,
    // so the first build using this only WATCHES.
    //
    // Inside, the delta the engine is about to apply is reached as
    //   rsi = [rcx+0x08]; rdi = [rsi]; selector = [rdi+0x8A]; delta = [rdi+0x20]
    // with the selector picking a whole-record write (gift) over a raw delta
    // (greet/dialogue) at 0x14239BC73.
    //
    // Verified: exactly one match in 2658, at 0x14239BC40.
    inline constexpr const char* kSig_FriendlyNpcInteract =
        "40 53 55 56 57 41 54 41 56 41 57 48 83 EC 30 48 8B 79 08";

    // The COMMIT half of the interaction, and the one that matters.
    //
    // NPC interaction is two calls, not one. The dispatcher validates first
    // (kSig_FriendlyNpcInteract, 0x1426E9AE0 in 2850) and commits afterwards
    // (0x1426E9C10) - paired at 0x1426BE5BA/0x1426BE76B and again at
    // 0x1426EA96A/0x1426EAA42. Both walk the same record array; only the
    // second adds the delta to durable trust.
    //
    // Trinity scaled the delta inside the VALIDATE hook and restored it the
    // moment that call returned, so the commit that reads it always saw the
    // original. The log said "N trust gain(s) multiplied" throughout, and it
    // was true in the narrowest sense: a value really was multiplied, then put
    // back before anything read it. That is why this never worked - on 2760
    // either. Scaling during validate is also the wrong side of the bounds
    // check that phase performs.
    //
    // Interior anchor on the record-array walk rather than the prologue: the
    // prologue is generic (wildcard the frame size and the same opening
    // matches five other functions) while this is the function doing its job.
    //     49 8B 5E 08     mov rbx, [r14+0x08]   ; kOff_NpcInteract_Data
    //     41 8B 46 10     mov eax, [r14+0x10]   ; kOff_NpcInteract_Count
    //     48 8D 04 C3     lea rax, [rbx+rax*8]  ; end = data + count*8
    //     48 89 45 ??     mov [rbp-0x31], rax
    //     48 3B D8        cmp rbx, rax
    // Unique in 2850, at 0x1426E9C60, inside 0x1426E9C10.
    inline constexpr const char* kSig_FriendlyNpcCommit =
        "49 8B 5E 08 41 8B 46 10 48 8D 04 C3 48 89 45 ?? 48 3B D8";

    // The ONE call that awards greet and dialogue trust, and the only place
    // Trinity can multiply it without killing the game.
    //
    // FriendlyAddDelta is what actually applies the award, and it cannot be
    // hooked: 0x141E2DA80 is a five-byte thunk into 0x14D88DD20, which lives
    // in the packed section with SPLIT RUNTIME_FUNCTION entries, so a
    // prologue detour corrupts stack unwinding - that is the crash users saw
    // on horseback, and why kHookTrustDelta has been false ever since. The
    // mount reaches the same function from its own call site (0x1428825B7),
    // so even a working hook would fire where it must not.
    //
    // This matches the call SITE inside the NPC interaction commit instead:
    //     4C 8B 4D 7F     mov   r9, [rbp+0x7F]       ; the delta
    //     44 0F B7 47 30  movzx r8d, word [rdi+0x30] ; group id
    //     48 8D 55 67     lea   rdx, [rbp+0x67]      ; &status
    //     E8 ?? ?? ?? ??  call  0x141E2DA80
    // with a fifth argument already homed at 0x1426E9D57 (`mov [rsp+0x20],
    // r12`), so the proxy takes five.
    //
    // Safe because 0x1426E9C10 has exactly two callers (0x1426BE76B and
    // 0x1426EAA42), both player-NPC interaction, and the mount reaches
    // neither. Displacements wildcarded; the register choice and the +0x30
    // group offset are the identity. Verified: one match in 2850, at
    // 0x1426E9D5C.
    inline constexpr const char* kSig_FriendlyCommitAddCall =
        "4C 8B 4D ?? 44 0F B7 47 30 48 8D 55 ?? E8";

    // The GREET reward's call into FriendlySetNpc, identified so the setter
    // hook can tell a reward from a load.
    //
    // FriendlySetNpc has exactly six callers, verified by scanning every E8 in
    // the image against the thunk at 0x141E2BB60:
    //     0x140712CB2  save-game load          -> must NOT scale
    //     0x14170051E  area streaming sync     -> must NOT scale
    //     0x1426E9BDF  dialogue commit         -> a reward
    //     0x1427A6B7D  gift commit             -> a reward (already scaled)
    //     0x1427A7576  gift commit             -> a reward (already scaled)
    //     0x14287B8E5  GREET reward            -> a reward, and the missing one
    //
    // Every one of them hands over a byte-identical 0x68 record, so the record
    // cannot say which it is; only the return address can. Trinity seeds a
    // first-sight relationship unscaled - correct for the load bursts, and
    // exactly wrong for a greet, which IS a first sight and never gets a
    // second call to scale on. That is why gifting worked (opening the menu
    // had already seeded the entry) and greeting never did.
    //
    //     48 8B 4E 68              mov rcx, [rsi+0x68]
    //     48 8D 55 80              lea rdx, [rbp-0x80]   ; a STACK record
    //     48 8B 89 40 01 00 00     mov rcx, [rcx+0x140]  ; player FriendlyComponent
    //     E8 ?? ?? ?? ??           call FriendlySetNpc
    //     66 41 3B DD              cmp bx, r13w
    //
    // Note `lea rdx, [rbp-0x80]`: the record is on the CALLER'S STACK and is
    // copied into the heap map by the callee. Anything that remembers that
    // pointer past the call is pointing at a dead frame - which is exactly how
    // an earlier attempt came to write into abandoned stack memory and report
    // success. Scale it here, in the call, or not at all.
    //
    // Verified: one match in 2850, at 0x14287B8D6.
    inline constexpr const char* kSig_FriendlyGreetSet =
        "48 8B 4E 68 48 8D 55 80 48 8B 89 40 01 00 00 E8 ?? ?? ?? ?? "
        "66 41 3B DD";
    // Byte offset of the instruction AFTER the call - what _ReturnAddress()
    // reports inside the hook.
    inline constexpr uintptr_t kOff_GreetSet_Ret = 0x14;

    // The SECOND reward caller, and the one the player's greets actually use.
    //
    // 0x1426E9AE0 is the interaction DISPATCHER: it walks the pending-reward
    // array (data at +0x08, count at +0x10, one record* every 8 bytes) and
    // applies each entry one of two ways - a raw delta through
    // FriendlyAddDelta when [rec+0x9B] is zero, or a whole-record set through
    // FriendlySetNpc/FriendlySetPet when it is not. Every record it touches is
    // a pending reward by construction, which is what makes its return
    // addresses safe to trust where a bare record is not.
    //
    // Trinity already knew this call site existed - the caller inventory above
    // lists 0x1426E9BDF as "dialogue commit" - and wired only 0x14287B8E5. A
    // live log settled which one greeting uses: three greets in one minute all
    // arrived from 0x1426E9BE4, and none from the address Trinity had.
    //
    // The interaction WINDOW that shipped before this could never have caught
    // them either. It was stamped on the COMMIT (0x1426E9C10) and the
    // dispatcher runs first, so by the time the clock was set the setter call
    // it was meant to cover had already returned. Not one record in a full
    // session logged as being inside the window, which is the measurement that
    // retired it.
    //
    //     48 8D 53 30              lea rdx, [rbx+0x30]   ; the record
    //     49 8B 06                 mov rax, [r14]
    //     48 8B 48 68              mov rcx, [rax+0x68]
    //     48 8B 89 40 01 00 00     mov rcx, [rcx+0x140]  ; player FriendlyComponent
    //     74 07                    je  +7                ; [rbx+0x99] picks which
    //     E8 ?? ?? ?? ??           call FriendlySetPet
    //     EB 05                    jmp +5
    //     E8 ?? ?? ?? ??           call FriendlySetNpc
    //
    // Verified: one match in 2850, at 0x1426E9BC4.
    inline constexpr const char* kSig_FriendlyRewardApply =
        "48 8D 53 30 49 8B 06 48 8B 48 68 48 8B 89 40 01 00 00 74 07 "
        "E8 ?? ?? ?? ?? EB 05 E8";
    // Instruction AFTER each of the two calls - what _ReturnAddress() reports
    // inside hkSetPet and hkSetNpc respectively.
    inline constexpr uintptr_t kOff_RewardApply_PetRet = 0x19; // 0x1426E9BDD
    inline constexpr uintptr_t kOff_RewardApply_NpcRet = 0x20; // 0x1426E9BE4
    // Offset of the 0xE8 inside that match.
    inline constexpr uintptr_t kOff_CommitAddCall_Call = 13;
    // Live-confirmed 2026-09-05 on build 2760: rcx+0x08 is NOT a pointer to a
    // record, it is a VECTOR's data pointer, with the element count at
    // rcx+0x10 and one record* every 8 bytes. Reading it as a single record
    // is what made the first probe say "record not readable" on most calls -
    // the vector is usually empty, and only a call that actually carries a
    // gain has an element in it. When one did, the delta read back as 1,
    // which is what a greet is worth.
    inline constexpr uintptr_t kOff_NpcInteract_Data     = 0x08; // record*[] data
    inline constexpr uintptr_t kOff_NpcInteract_Count    = 0x10; // u32 element count
    // Sanity bound on the count before we walk it, and a cap on how many
    // records one call may borrow - both exist so a wrong offset produces a
    // no-op rather than a walk through arbitrary memory.
    inline constexpr uint32_t  kNpcInteract_MaxCount     = 256;
    inline constexpr int       kNpcInteract_MaxScaled    = 16;
    // The 0x8A selector this once carried is REMOVED. It was inferred, never
    // observed - the probe that would have confirmed it never ran, because the
    // game patched first - and in 2760 the gate at this point in the
    // dispatcher is a byte at record+0x9B, not +0x8A. Rather than carry an
    // unverified offset that a probe would then print with confidence, the
    // probe reports only the delta, which is not inferred at all: the
    // dispatcher loads it into r9 as the accumulator's argument, in plain
    // sight at `mov r9,[rbx+0x20]`.
    inline constexpr uintptr_t kOff_NpcInteractRec_Delta = 0x20; // i64 gain

    inline constexpr uintptr_t kOff_FriendlyRec_Key   = 0x00; // u32 record key
    inline constexpr uintptr_t kOff_FriendlyRec_Group = 0x04; // u16 group/bucket key
    inline constexpr uintptr_t kOff_FriendlyRec_Value = 0x20; // i64 trust value
    inline constexpr int64_t   kFriendly_Max          = 100;  // the taming cap

    // The live relationship object handed to kSig_FriendlyAddDelta as ARG1 -
    // a different shape from the 0x58-byte record above. Derived from the
    // function's own prologue (see kSig_FriendlyAddDelta): u32 tier at +0x00,
    // i64 trust at +0x08. Needed because the accumulator has to be capped
    // against the CURRENT value, not against the delta alone.
    inline constexpr uintptr_t kOff_FriendlyRel_Trust = 0x08; // i64 current trust
    // How long the trust-record stream has to be quiet before Friendly::Tick
    // flushes its burst summary to the log. An area load pushes every nearby
    // relationship through in well under a second, so half a second groups a
    // burst into one line without ever splitting one.
    inline constexpr uint64_t  kFriendlyBurst_QuietMs = 500;

    // 1.18.02 encodes the SAME function differently, and the pattern above
    // cannot match it at any offset. `44 8B 41 ?` is the disp8 form of
    // `mov r8d,[rcx+disp]`; this build needs disp32 - `44 8B 81 80 00 00 00` -
    // which is a different ENCODING, not a different number.

    // --- Durability / Repair ---------------------------------------------------
    // 0x3F0 before TU 2.02.00. The ItemInfo row grew by 16 bytes ahead of this
    // field, which no byte signature can see: the deserialiser proves it at
    // 0x14147EDC5 `lea rdx, [rsi + 0x400]`, immediately before the error string
    // for _maxEndurance. Trinity reads it in MaxEnduranceForType, where a stale
    // offset does not fail loudly - it reads a neighbouring field and either
    // trips the 0/0xFFFF guard or, worse, returns a plausible wrong cap.
    inline constexpr uintptr_t kOff_ItemDef_MaxEndurance = 0x400;   // u16, ItemInfo row
    inline constexpr uintptr_t kOff_ItemVal_Endurance    = 0x40;    // u16, live item value
    inline constexpr uint16_t  kEndurance_None           = 0xFFFF;  // "this item has none"
    // 0x3F8 before TU 2.02.00, moved with its neighbour above - proof at
    // 0x14147EDE5 `lea rdx, [rsi + 0x408]`. Nothing reads it yet; it is kept
    // because it is how the endurance field's new home was pinned down, and
    // the next person to look will want the row's shape, not one offset.
    inline constexpr uintptr_t kOff_ItemDef_RepairDataList = 0x408; // vector

    // How many slots Add Item quietly expands a storage to when it is about to overflow.
    inline constexpr int kAddRoom_TargetSlots = 2000;

    // Warp landing.
    inline constexpr float    kWarp_RiseAbove = 30.0f;
    inline constexpr unsigned kWarp_GraceMs   = 8000;
}
