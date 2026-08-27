#include "../core/version.h"
#include "../core/logger.h"
#include "../core/i18n.h"
#include "menu.h"

#include <imgui.h>
#include <cstdio>
#include <cstring>
#include <vector>

#include <Windows.h>
#include <Xinput.h>

#include "framework.h"
#include "widgets.h"
#include "../core/settings.h"
#include "../core/state.h"
#include "../core/text.h"
#include "../game/parry.h"
#include "../game/player.h"
#include "../game/teleport.h"
#include "../game/inventory.h"
#include "../game/world.h"
#include "../game/weather.h"
#include "../game/dye.h"
#include "../game/dye_data.h" // the game's dye families / preset shades (generated)
#include "../game/equipment.h"
#include "../game/friendly.h"

namespace trinity::gui
{
    // Top-level sections. Always visible as the tab strip; Q/E/Tab or LB/RB
    // jump between them from anywhere, so nothing is ever more than a press
    // or two away.
    static const char* const kTabs[] = { "PLAYER", "TRAVEL", "INVENTORY", "WORLD", "SYSTEM" };
    enum Tab { TabPlayer, TabTravel, TabInventory, TabWorld, TabSystem, TabCount };

    bool WantsDraw()
    {
        const State& st = State::Get();
        return st.menuOpen || st.showFps || ui::ToastsActive();
    }

    static void DrawFpsCounter()
    {
        ImGuiIO&    io = ImGui::GetIO();
        ImDrawList* dl = ImGui::GetForegroundDrawList();

        char buf[64];
        // While Free Flight is on, light "FLY" whenever a direction is actively
        // driving your height (a fly key/button held while airborne), so it's
        // obvious when the controls have taken over. Harmless otherwise - FPS.
        if (State::Get().freeFlight)
        {
            const bool fly = game::Teleport::GetFlightEngaged();
            snprintf(buf, sizeof(buf), "%.0f FPS%s", io.Framerate, fly ? "  FLY" : "");
        }
        else
            snprintf(buf, sizeof(buf), "%.0f FPS", io.Framerate);

        const float  sz  = ImGui::GetFontSize();
        const ImVec2 ts  = ImGui::GetFont()->CalcTextSizeA(sz, 3.402823466e+38f, 0.0f, buf);
        const float  pad = sz * 0.4f;
        const ImVec2 mn(io.DisplaySize.x - ts.x - pad * 2.0f - sz, sz);
        const ImVec2 mx(mn.x + ts.x + pad * 2.0f, mn.y + ts.y + pad * 2.0f);

        dl->AddRectFilled(mn, mx, IM_COL32(7, 7, 9, 220));
        dl->AddText(ImGui::GetFont(), sz, ImVec2(mn.x + pad, mn.y + pad),
                    IM_COL32(214, 36, 56, 255), buf);
    }

    // --- Tab pages -----------------------------------------------------------

    static void RenderPlayer()
    {
        State& st = State::Get();
        ui::Begin();

        ui::Submenu("Dye Equipment", "dyeslots",
                    game::Dye::Ready()
                        ? "Recolor your equipped gear."
                        : "Recolor your equipped gear. Load into the world first.");

        ui::Submenu("Edit Equipment", "equipslots",
                    game::Equipment::Ready()
                        ? "Refine your gear and socket abyss gears into it."
                        : "Refine and socket your gear. Load into the world first.");

        bool changed = false;
        changed |= ui::Toggle("God Mode", &st.godMode,
                   game::Player::Ready()
                       ? "Keeps your health full."
                       : "Keeps your health full. Load into the game world first.");
        changed |= ui::Toggle("Infinite Stamina", &st.infStamina,
                   "Keeps player and mount stamina full.");
        changed |= ui::Toggle("Infinite Spirit", &st.infSpirit,
                   "Keeps your spirit full.");
        changed |= ui::Toggle("Immune to Fire & Heat", &st.immuneFire,
                   "Blocks fire damage, burning status, and desert/lava heat buildup.");
        changed |= ui::Toggle("Immune to Cold & Frost", &st.immuneCold,
                   "Blocks cold damage, hypothermia status, freezing, and snow stamina drain.");
        changed |= ui::ToggleFloat("Super Run", &st.superRun, &st.superRunMult, 1.0f, 10.0f, 0.25f, 2.0f, "%.2fx",
                        "Move faster than normal.");
        changed |= ui::ToggleFloat("Super Jump", &st.superJump, &st.superJumpMult, 1.0f, 10.0f, 0.25f, 2.0f, "%.2fx",
                        "Jump higher than normal.");
        changed |= ui::ToggleFloat("Free Flight", &st.freeFlight, &st.flightSpeed, 1.0f, 40.0f, 1.0f, 8.0f, "%.0f",
                        "While airborne, hold Caps Lock / RB to rise or Ctrl / Right Trigger "
                        "to sink. Let go and normal physics resume - jumps and aerial attacks "
                        "are untouched.");
        changed |= ui::ToggleFloat("Trust Multiplier", &st.trustMult, &st.trustMultVal, 1.0f, 25.0f, 0.25f, 3.0f, "%.2fx",
                        game::Friendly::Ready()
                            ? "Gifting NPCs or feeding animals builds trust faster."
                            : "Gifting NPCs or feeding animals builds trust faster. Unavailable right now.");
        if (ui::Toggle("Easy Parry", &st.easyParry,
                       game::Parry::Available()
                           ? "Removes the timing-margin test on a parry. You still have to parry the "
                             "attack - it just no longer has to be frame-perfect."
                           : "Removes the timing-margin test on a parry. Unavailable right now."))
        {
            game::Parry::SetEnabled(st.easyParry);
            changed = true;
        }
        changed |= ui::Toggle("No Fall Damage", &st.noFallDamage,
                        "Stops damage that arrives with no attacker behind it, which is what a fall is.");
        changed |= ui::Toggle("One-Hit Kill", &st.oneHitKill,
                        "Anything you hit dies. Overrides the Outgoing Damage slider.");
        changed |= ui::FloatOption("Outgoing Damage", &st.dmgOutMult, 0.0f, 20.0f, 0.25f, 1.0f, "%.2fx",
                        st.oneHitKill ? "Adjusts how much damage you deal. One-Hit Kill is on, so this is ignored."
                                      : "Adjusts how much damage you deal.");
        changed |= ui::FloatOption("Incoming Damage", &st.dmgInMult, 0.0f, 10.0f, 0.25f, 1.0f, "%.2fx",
                        "Adjusts how much damage you take.");

        if (changed && st.autoSave)
            Settings::Save();
        ui::End();
    }

    // --- Dye editor -----------------------------------------------------------
    // PLAYER -> Dye Equipment -> (equipped piece) -> preset swatches, with
    // custom RGB one level down. Applies through the game's own dyehouse
    // client path (see game/dye.h), so a picked color renders instantly and
    // persists like a real dye job - which is why the presets need no
    // separate Apply step: the swatch IS the apply.
    //
    // The presets are the dyehouse's own palette (dye_data.h): ten families,
    // each 9 neutral tones + a 10x10 shade grid, exactly what the in-game
    // dye UI offers. The family key is written into the record so the game's
    // own UI files the color correctly; custom RGB reuses the same path with
    // no palette restriction.

    static uint16_t s_dyeTag = 0;    // selected slot's engine tag
    static char s_dyeItem[64];       // its item name - the edit pages' title
    static int s_dyeChan   = 0;      // 0 = all zones, 1..12 = one zone
    static int s_dyeFamily = 0;      // index into kDyeFamilies
    static int s_dyeR = 200, s_dyeG = 30, s_dyeB = 40; // the custom mix
    static int s_dyeMat    = 0;      // 0 = natural, 1..10 = engine material template
    static int s_dyeRepair = 100;    // 100 = pristine .. 0 = battle-worn
    static int s_dyeCursor[1 + game::kDyeGridRows]; // swatch focus per grid row

    // Poll the queued apply for its one-shot outcome (Status is read-and-clear).
    static void ReportPendingDye()
    {
        const game::Dye::OpState s = game::Dye::Status();
        if (s == game::Dye::OpState::Done)
            ui::Toast("Dye applied");
        else if (s == game::Dye::OpState::Failed)
            ui::Toast("Could not dye that - see the log");
    }

    static void SendDye(uint32_t familyKey, int r, int g, int b)
    {
        game::Dye::Channel c{};
        c.groupKey   = familyKey;
        c.r          = static_cast<uint8_t>(r);
        c.g          = static_cast<uint8_t>(g);
        c.b          = static_cast<uint8_t>(b);
        c.materialId = (s_dyeMat == 0) ? uint16_t(0xFFFF) : static_cast<uint16_t>(s_dyeMat);
        c.repair     = static_cast<uint8_t>(((100 - s_dyeRepair) * 127) / 100);
        if (game::Dye::Apply(s_dyeTag, s_dyeChan - 1, c))
            ui::Toast("Applying dye...");
        // The custom rows follow whatever was applied last, so the Custom
        // Color page always opens on the color the item just got.
        s_dyeR = r; s_dyeG = g; s_dyeB = b;
    }

    // Material / condition edits on an already-dyed zone re-apply that zone's
    // own color with the new settings, so scrubbing the value previews live.
    // Debounced (the queue takes one request at a time), single-zone only: an
    // "all zones" retouch would repaint every zone with zone 1's color.
    static bool      s_dyeRetouch   = false;
    static ULONGLONG s_dyeRetouchAt = 0;

    static void PumpDyeRetouch()
    {
        if (!s_dyeRetouch || GetTickCount64() - s_dyeRetouchAt < 350)
            return;
        if (s_dyeChan == 0) { s_dyeRetouch = false; return; }

        game::Dye::Channel cc{};
        if (!game::Dye::GetChannel(s_dyeTag, s_dyeChan - 1, &cc))
        {
            // Nothing dyed here yet - the settings ride along with the next
            // color pick instead.
            s_dyeRetouch = false;
            return;
        }
        game::Dye::Channel c{};
        c.groupKey   = cc.groupKey;
        c.r = cc.r; c.g = cc.g; c.b = cc.b;
        c.materialId = (s_dyeMat == 0) ? uint16_t(0xFFFF) : static_cast<uint16_t>(s_dyeMat);
        c.repair     = static_cast<uint8_t>(((100 - s_dyeRepair) * 127) / 100);
        if (game::Dye::Apply(s_dyeTag, s_dyeChan - 1, c))
            s_dyeRetouch = false; // else the queue was busy - retry next frame
    }

    static void RenderDyeSlots()
    {
        ui::Begin();

        if (!game::Dye::Ready())
        {
            ui::Option("Waiting for your equipment...",
                       "Load into the world - if this persists, change any "
                       "equipment piece once so the mod can see your gear.");
            ui::End();
            return;
        }

        const int n = game::Dye::SlotCount();
        int shown = 0, hidden = 0;
        char hiddenList[200] = "";

        for (int i = 0; i < n; ++i)
        {
            game::Dye::SlotInfo si{};
            if (!game::Dye::GetSlot(i, &si)) continue;

            // Pieces with no dye channels are collected into one summary row
            // below instead of cluttering the list with dead ends.
            if (!si.dyeable)
            {
                const size_t len = strlen(hiddenList);
                snprintf(hiddenList + len, sizeof(hiddenList) - len, "%s%s",
                         hidden ? ", " : "", si.itemName);
                ++hidden;
                continue;
            }
            ++shown;

            char label[160];
            if (si.dyeCount > 0)
                snprintf(label, sizeof(label), "%s - %s  (%u dyed)",
                         si.slotName, si.itemName, si.dyeCount);
            else
                snprintf(label, sizeof(label), "%s - %s", si.slotName, si.itemName);

            if (ui::SubmenuItem(label, si.icon[0] ? si.icon : nullptr, "dyeedit",
                                "Recolor this piece."))
            {
                // A different piece gets fresh pages (selection, scroll); the
                // same piece keeps them, so hopping out and back in is free.
                if (s_dyeTag != si.tag || strcmp(s_dyeItem, si.itemName) != 0)
                {
                    ui::ResetMenu("dyeedit");
                    ui::ResetMenu("dyecustom");
                    s_dyeRetouch = false;
                }
                s_dyeTag = si.tag;
                snprintf(s_dyeItem, sizeof(s_dyeItem), "%s", si.itemName);
            }
        }

        if (n == 0)
            ui::Option("Nothing equipped", "Equip some gear first.");
        else if (shown == 0)
            ui::Option("Nothing dyeable equipped", "None of these pieces can be dyed.");

        if (hidden > 0)
        {
            char label[48];
            snprintf(label, sizeof(label), "Can't be dyed: %d piece%s",
                     hidden, hidden == 1 ? "" : "s");
            char desc[256];
            snprintf(desc, sizeof(desc), "%s", hiddenList);
            ui::Option(label, desc);
        }

        ui::End();
    }

    static void RenderDyeEdit()
    {
        ui::Begin(s_dyeItem[0] ? s_dyeItem : nullptr);

        static const char* const kChanItems[] = {
            "All zones", "Zone 1", "Zone 2", "Zone 3", "Zone 4", "Zone 5", "Zone 6",
            "Zone 7", "Zone 8", "Zone 9", "Zone 10", "Zone 11", "Zone 12"
        };
        static const char* s_famItems[game::kDyeFamilyCount];
        static bool s_famInit = false;
        if (!s_famInit)
        {
            for (int i = 0; i < game::kDyeFamilyCount; ++i)
                s_famItems[i] = game::kDyeFamilies[i].name;
            s_famInit = true;
        }

        ui::Combo("Dye Zone", &s_dyeChan, kChanItems, 13,
                  "Which part of the item to color. Most gear only uses the first few.");
        ui::Combo("Color Family", &s_dyeFamily, s_famItems, game::kDyeFamilyCount,
                  "Pick a color family to browse its shades below.");

        const game::DyeFamily& fam = game::kDyeFamilies[s_dyeFamily];

        // The zone's current color, marked with a dot on its swatch below.
        game::Dye::Channel cur{};
        const bool haveCur = game::Dye::GetChannel(
            s_dyeTag, (s_dyeChan == 0) ? 0 : s_dyeChan - 1, &cur);

        // Row 0: the family's 9 neutral tones, led by a "remove dye" swatch so
        // the row is 10 wide like the rest and clearing lives right in the
        // palette; rows 1..10: the 10x10 grid, darker down the rows, richer to
        // the right - the dyehouse's own layout. Picking a swatch acts at once.
        for (int row = 0; row <= game::kDyeGridRows; ++row)
        {
            const bool neutral = (row == 0);
            const int  lead    = neutral ? 1 : 0; // the remove swatch
            const int  base    = neutral ? 0
                               : game::kDyeNeutrals + (row - 1) * game::kDyeGridCols;
            const int  shades  = neutral ? game::kDyeNeutrals : game::kDyeGridCols;
            const int  cnt     = lead + shades;

            uint32_t rgb[game::kDyeGridCols] = {};
            int      mark = -1;
            for (int i = 0; i < shades; ++i)
            {
                const game::DyeShadeRGB& sh = fam.shades[base + i];
                rgb[lead + i] = (uint32_t(sh.r) << 16) | (uint32_t(sh.g) << 8) | sh.b;
                if (haveCur && sh.r == cur.r && sh.g == cur.g && sh.b == cur.b)
                    mark = lead + i;
            }

            const int hit = ui::SwatchRow("", rgb, cnt, &s_dyeCursor[row], mark,
                neutral ? "Pick a tone, or the first swatch to remove the dye."
                        : "Pick a color to dye it right away.",
                neutral ? 0 : -1);
            if (hit >= 0)
            {
                if (neutral && hit == 0)
                {
                    if (game::Dye::Clear(s_dyeTag, s_dyeChan - 1))
                        ui::Toast("Removing dye...");
                }
                else
                {
                    const game::DyeShadeRGB& sh = fam.shades[base + hit - lead];
                    SendDye(fam.key, sh.r, sh.g, sh.b);
                }
            }
        }

        ui::Submenu("Custom Color", "dyecustom", "Mix your own color instead of a preset.");

        bool touched = false;
        touched |= ui::IntOption("Material", &s_dyeMat, 0, 10, 1, 0,
                      "Swap the fabric or metal look. 0 keeps it natural.");
        touched |= ui::IntOption("Condition %", &s_dyeRepair, 0, 100, 5, 100,
                      "How worn the piece looks. 100 is pristine, 0 is battle-scarred.");
        if (touched && s_dyeChan != 0)
        {
            s_dyeRetouch   = true;
            s_dyeRetouchAt = GetTickCount64();
        }
        PumpDyeRetouch();

        ui::End();
    }

    static void RenderDyeCustom()
    {
        ui::Begin(s_dyeItem[0] ? s_dyeItem : nullptr);

        ui::IntOption("Red",   &s_dyeR, 0, 255, 5, 200, "Red 0-255.");
        ui::IntOption("Green", &s_dyeG, 0, 255, 5, 30,  "Green 0-255.");
        ui::IntOption("Blue",  &s_dyeB, 0, 255, 5, 40,  "Blue 0-255.");

        const uint32_t mix = (uint32_t(s_dyeR) << 16) |
                             (uint32_t(s_dyeG) << 8)  | uint32_t(s_dyeB);
        static int s_applyCursor = 0;
        if (ui::SwatchRow("Apply This Color", &mix, 1, &s_applyCursor, -1,
                          "Dye it with this exact color.") == 0)
            SendDye(game::kDyeFamilies[s_dyeFamily].key, s_dyeR, s_dyeG, s_dyeB);

        if (ui::Option("Load Current", "Load the zone's current color."))
        {
            const int ch = (s_dyeChan == 0) ? 0 : s_dyeChan - 1;
            game::Dye::Channel c{};
            if (game::Dye::GetChannel(s_dyeTag, ch, &c))
            {
                s_dyeR = c.r; s_dyeG = c.g; s_dyeB = c.b;
                s_dyeMat    = (c.materialId == 0xFFFF || c.materialId > 10) ? 0 : c.materialId;
                s_dyeRepair = (c.repair == 0xFF) ? 100 : 100 - (c.repair * 100 + 63) / 127;
                for (int i = 0; i < game::kDyeFamilyCount; ++i)
                    if (game::kDyeFamilies[i].key == c.groupKey) { s_dyeFamily = i; break; }
                ui::Toast("Loaded zone %d", ch + 1);
            }
            else
            {
                ui::Toast("That zone has no dye yet");
            }
        }

        ui::End();
    }

    // --- Equipment editor (abyss-gear sockets) --------------------------------
    // PLAYER -> Edit Equipment -> (equipped piece) -> per-socket gear picker.
    // Adding/clearing a gear writes both realms and persists like dye; unlocking
    // sockets renders this session only (see game/equipment.h).

    static uint16_t s_eqTag = 0xFFFF; // selected piece's engine tag
    static char     s_eqItem[64] = "";// its item name - the edit/picker title
    static int      s_eqSocket = 0;   // socket index the picker is editing
    static char     s_eqFind[48] = "";// gear picker search
    static int      s_eqRefine = 0;   // refinement stepper value (seeded on select)

    // Locate the live snapshot slot for a tag (SlotCount() rebuilds it first).
    static bool EqSlotForTag(uint16_t tag, game::Equipment::SlotInfo* out)
    {
        const int n = game::Equipment::SlotCount();
        for (int i = 0; i < n; ++i)
            if (game::Equipment::GetSlot(i, out) && out->tag == tag)
                return true;
        return false;
    }

    static void RenderEquipSlots()
    {
        ui::Begin();

        if (!game::Equipment::Ready())
        {
            ui::Option("Waiting for your equipment...", "Load into the world first.");
            ui::End();
            return;
        }

        // Whole-loadout actions first: they are the reason most people open this
        // page, and doing them piece by piece is the tedious part.
        {
            static char s_eqBatchMsg[128] = "";
            const bool durable = game::Equipment::EditsPersist();

            if (ui::Option("Max Refine All (+10)",
                           durable ? "Refines every worn piece to +10."
                                   : "Refines every worn piece to +10. Server copy not "
                                     "resolved yet, so this may not survive a reload."))
            {
                bool all = false;
                const int k = game::Equipment::RefineAllMax(&all);
                snprintf(s_eqBatchMsg, sizeof(s_eqBatchMsg),
                         k == 0 ? "Nothing to refine."
                                : (all ? "Refined %d piece(s) to +10."
                                       : "Refined %d piece(s) to +10 - some are this session only."),
                         k);
            }

            // Repair All Gear / Repair Everything in Bags are NOT exposed.
            //
            // The offsets are almost certainly right - a probe showed the live
            // value at +0x40 and the row maximum at +0x3F0 BOTH reading 0xFFFF
            // on all 20 worn pieces and 636 of 637 carried items. Those two
            // agreeing is the game saying these items have no durability at
            // all, not a bad read. Until something in this game is found that
            // actually degrades, a "Repair" button has nothing to repair, so it
            // stays out of the menu. Equipment::RepairAllWorn and
            // Inventory::RepairAllCarried are kept and still correct.

            if (ui::Option("Unlock All Sockets",
                           "Opens all five sockets on every worn piece."))
            {
                const int k = game::Equipment::UnlockAllSockets();
                snprintf(s_eqBatchMsg, sizeof(s_eqBatchMsg),
                         k == 0 ? "Nothing to unlock." : "Opened the sockets on %d piece(s).", k);
            }

            if (ui::Option("Remove All Abyss Gear",
                           "Empties every socket on every worn piece. The sockets stay open."))
            {
                bool all = false;
                const int k = game::Equipment::ClearAllGears(&all);
                snprintf(s_eqBatchMsg, sizeof(s_eqBatchMsg),
                         k == 0 ? "Nothing to remove." : "Emptied the sockets on %d piece(s).", k);
            }

            if (s_eqBatchMsg[0]) ui::Option(s_eqBatchMsg);
        }

        const int n = game::Equipment::SlotCount();
        for (int i = 0; i < n; ++i)
        {
            game::Equipment::SlotInfo si{};
            if (!game::Equipment::GetSlot(i, &si)) continue;

            char label[176];
            if (si.unlockedCount > 0)
                snprintf(label, sizeof(label), "%s - %s  (%d/%d socket%s used)",
                         si.slotName, si.itemName, si.filledCount, si.unlockedCount,
                         si.unlockedCount == 1 ? "" : "s");
            else
                snprintf(label, sizeof(label), "%s - %s  (no sockets)",
                         si.slotName, si.itemName);

            if (ui::SubmenuItem(label, si.icon[0] ? si.icon : nullptr, "equipedit",
                                "Refine this piece and edit its abyss-gear sockets."))
            {
                // A different piece gets a fresh picker page.
                if (s_eqTag != si.tag || strcmp(s_eqItem, si.itemName) != 0)
                    ui::ResetMenu("equipgear");
                s_eqTag = si.tag;
                s_eqRefine = si.refineLevel; // seed the stepper from the live level
                snprintf(s_eqItem, sizeof(s_eqItem), "%s", si.itemName);
            }
        }
        if (n == 0)
            ui::Option("Nothing equipped", "Equip some gear first.");

        ui::End();
    }

    static void RenderEquipEdit()
    {
        ui::Begin(s_eqItem[0] ? s_eqItem : nullptr);

        game::Equipment::SlotInfo si{};
        if (!EqSlotForTag(s_eqTag, &si))
        {
            ui::Option("Not equipped", "This piece is no longer equipped.");
            ui::End();
            return;
        }

        if (si.unlockedCount < game::Equipment::kMaxSockets)
        {
            if (ui::Option("Unlock all sockets",
                           "Opens every socket on this piece (up to 5), then socket gears into them."))
            {
                if (game::Equipment::UnlockAll(si.tag))
                    ui::Toast("All sockets unlocked");
                else
                    ui::Toast("Could not unlock - see the log");
            }
        }
        if (si.filledCount > 0)
        {
            if (ui::Option("Clear all sockets",
                           "Removes every abyss gear from this piece, leaving the sockets open."))
            {
                if (game::Equipment::ClearAll(si.tag))
                    ui::Toast("All sockets cleared");
                else
                    ui::Toast("Could not clear - see the log");
            }
        }

        // Refinement (0..10) - applies to every piece, sockets or not, so it sits
        // above the socket-only early-out. Left/Right steps the level; each change
        // writes both realms and persists.
        {
            int lvl = s_eqRefine;
            if (ui::IntOption("Refinement", &lvl, 0, game::Equipment::kRefineMax, 1, si.refineLevel,
                              "Refine this piece from 0 to 10."))
            {
                s_eqRefine = lvl;
                bool p = false;
                if (game::Equipment::SetRefine(si.tag, lvl, &p))
                    ui::Toast(p ? "Refinement set to %d" : "Refinement %d (this session)", lvl);
                else
                    ui::Toast("Could not set refinement - see the log");
            }
        }

        if (si.unlockedCount == 0)
        {
            ui::Option("No sockets yet", "Unlock sockets above to add abyss gears.");
            ui::End();
            return;
        }

        if (!game::Equipment::EditsPersist())
            ui::Option("Note: not saving yet",
                       "Your save is still loading - gear edits apply visually but revert "
                       "on reload until this clears.");

        for (int k = 0; k < si.unlockedCount; ++k)
        {
            const game::Equipment::Socket& so = si.sockets[k];
            char label[112];
            snprintf(label, sizeof(label), "Socket %d: %s", k + 1,
                     so.filled ? so.gearName : "Empty");
            if (ui::SubmenuItem(label, (so.filled && so.gearIcon[0]) ? so.gearIcon : nullptr,
                                "equipgear",
                                so.filled ? "Change or remove this abyss gear."
                                          : "Add an abyss gear to this socket."))
            {
                s_eqSocket = k;
                s_eqFind[0] = 0;
                ui::ResetMenu("equipgear");
            }
        }

        ui::End();
    }

    static void RenderEquipGear()
    {
        char title[112];
        snprintf(title, sizeof(title), "%s - Socket %d", s_eqItem, s_eqSocket + 1);
        ui::Begin(title);

        // Clearing the socket is always the first choice (icon box left empty so
        // it lines up with the gear rows below).
        if (ui::OptionItem("- Empty this socket -", nullptr, "Remove whatever gear is in this socket."))
        {
            bool p = false;
            if (game::Equipment::ClearGear(s_eqTag, s_eqSocket, &p))
            {
                ui::Toast(p ? "Socket cleared" : "Socket cleared (this session)");
                ui::PopMenu();
            }
            else
                ui::Toast("Could not clear - see the log");
            ui::End();
            return;
        }

        const int total = game::Equipment::GearCount();
        if (total == 0)
        {
            ui::Option("No abyss gears found",
                       "The game's Abyss Gear catalog did not resolve.");
            ui::End();
            return;
        }

        ui::Search(s_eqFind, sizeof(s_eqFind), "Find an abyss gear by name.");

        int shown = 0;
        for (int i = 0; i < total && shown < 200; ++i)
        {
            uint16_t    tid  = 0;
            const char* name = nullptr;
            const char* icon = nullptr;
            if (!game::Equipment::GetGear(i, &tid, &name, &icon)) continue;
            if (s_eqFind[0] && !ContainsNoCase(name, s_eqFind)) continue;
            ++shown;

            if (ui::OptionItem(name, (icon && icon[0]) ? icon : nullptr, "Socket this abyss gear."))
            {
                bool p = false;
                if (game::Equipment::AddGear(s_eqTag, s_eqSocket, tid, &p))
                {
                    ui::Toast(p ? "Socketed %s" : "Socketed %s (this session)", name);
                    ui::PopMenu();
                }
                else
                    ui::Toast("Could not socket that - see the log");
                ui::End();
                return;
            }
        }
        if (shown == 0)
            ui::Option("No matches", "No abyss gear is called that.");
        else if (shown >= 200)
            ui::Option("More matches...", "Only the first 200 are listed - keep typing to narrow it down.");

        ui::End();
    }

    static void RenderWorld()
    {
        State& st = State::Get();
        ui::Begin();

        const bool ready = game::World::Ready();
        bool changed = false;
        changed |= ui::ToggleFloat("Game Speed", &st.gameSpeed, &st.gameSpeedMult, 0.1f, 5.0f, 0.05f, 1.0f, "%.2fx",
                   ready
                       ? "Speeds up or slows down the game."
                       : "Speeds up or slows down the game. Unavailable right now.");

        const bool timeReady = game::World::TimeOfDayReady();
        changed |= ui::Toggle("Freeze Time of Day", &st.timeFrozen,
                   timeReady
                       ? "Holds the clock at the current time; the world keeps running normally."
                       : "Holds the clock in place. Unavailable right now.");

        // Advance the clock by however many hours the user dials in. Left/Right
        // step the amount (held: x10), Enter types an exact one, then the same
        // press applies it (see ui::IntAction).
        static int s_advHours = 1;
        if (ui::Toggle("No Bounty", &st.noBounty,
                       "Crimes and theft stop adding bounty, regional marking, or reducing "
                       "faction contribution. Session only; changes no save data."))
        {
            game::Inventory::SetNoBounty(st.noBounty);
            changed = true;
        }

        // Weather is NOT exposed here, and that is deliberate.
        //
        // The machinery works: weather.cpp hooks the preset deserialiser,
        // captures every preset the world loads (77 in a field test) and
        // re-stamps them. It changes nothing you can see. Advancing the clock
        // six hours did not make the edited presets take either, so the sky is
        // not blended from this data at all - it comes from the live weather
        // simulator component, which this mod cannot reach yet.
        //
        // A toggle that promises weather and delivers none is the same trap as
        // the old "Find My Silver" row, so it stays out of the menu until the
        // live path is found. The code and the research are kept - see
        // weather.h and offsets.h - because the layout was expensive to get
        // and is entirely correct; only the destination is wrong.

        if (ui::IntAction("Advance Time", &s_advHours, 1, 240, 1, 1,
                   timeReady
                       ? "Skips the clock forward by this many hours; time keeps flowing after."
                       : "Skips the clock forward. Unavailable right now."))
        {
            if (game::World::AdvanceTimeOfDayHours(s_advHours))
                ui::Toast("Advanced %d hour%s", s_advHours, s_advHours == 1 ? "" : "s");
        }

        if (changed && st.autoSave)
            Settings::Save();

        ui::End();
    }

    // Saved locations. Coordinates the player chose to keep, with a name.
    // Deliberately built on the same warp the marker teleport will use later,
    // so that feature becomes "find the marker's X/Z" rather than a whole new
    // subsystem.
    static char s_bmName[48] = "";
    static int  s_bmArmed = -1;   // index armed for removal, -1 = none

    static void RenderSavedLocations()
    {
        ui::Begin();
        ui::ListJump();

        float x = 0.0f, y = 0.0f, z = 0.0f;
        float originX = 0.0f, originY = 0.0f, originZ = 0.0f;
        const bool inWorld = game::Teleport::GetLastPosition(
            &x, &y, &z, &originX, &originY, &originZ);

        // Name first, then save. The order matters: the first version put the
        // name field underneath and labelled it "Search", so nobody could tell
        // it was a name field at all.
        ui::Search(s_bmName, sizeof(s_bmName),
                   "Optional. Leave it blank and the spot is numbered for you.",
                   "Name This Spot");

        if (ui::Option(inWorld ? "Save This Spot" : "Save This Spot (not in the world yet)",
                       inWorld ? "Adds where you are standing to the list below."
                               : "Load into the world and take a step first."))
        {
            if (game::Teleport::AddBookmarkHere(s_bmName[0] ? s_bmName : nullptr))
            {
                ui::Toast("Saved %s", s_bmName[0] ? s_bmName : "this spot");
                s_bmName[0] = 0;
            }
            else
            {
                ui::Toast("Could not save - list is full or you are not in the world");
            }
        }

        const size_t n = game::Teleport::BookmarkCount();
        if (n == 0)
        {
            ui::Option("Nothing saved yet",
                       "Stand somewhere worth returning to and press Save This Spot.");
            ui::End();
            return;
        }

        for (size_t i = 0; i < n; ++i)
        {
            char  nm[48] = {};
            float bx = 0, by = 0, bz = 0;
            if (!game::Teleport::GetBookmark(i, nm, sizeof(nm), &bx, &by, &bz)) continue;

            char label[96];
            snprintf(label, sizeof(label), "%s", nm);
            char desc[160];
            if (inWorld)
                snprintf(desc, sizeof(desc),
                         "X %.0f  Y %.0f  Z %.0f - press to travel, Del to remove. "
                         "Available until this game session ends.",
                         bx - originX, by - originY, bz - originZ);
            else
                snprintf(desc, sizeof(desc),
                         "Load into the world to travel. Available until this game session ends.");

            // Removal is armed by one Del and confirmed by a second, the same
            // rule the item editor uses - these are worth a moment's pause,
            // since a lost coordinate cannot be recovered.
            const bool armed = (s_bmArmed == static_cast<int>(i));
            if (armed) snprintf(desc, sizeof(desc), "Press Del again to remove %s.", nm);

            bool del = false;
            if (ui::OptionRemovable(armed ? "Remove this location?" : label, desc, &del))
            {
                s_bmArmed = -1;
                if (!inWorld)
                    ui::Toast("Load into the world first");
                else if (game::Teleport::WarpToBookmark(i))
                    ui::Toast("Travelling to %s", nm);
                else
                    ui::Toast("Another warp is still active");
            }
            else if (del)
            {
                if (armed)
                {
                    s_bmArmed = -1;
                    if (game::Teleport::DeleteBookmark(i))
                    {
                        ui::Toast("Removed %s", nm);
                        break;                 // the list just shifted under us
                    }
                }
                else
                {
                    s_bmArmed = static_cast<int>(i);
                }
            }
        }

#if TRINITY_MARKER_RESEARCH
        // Locating the destination pin in memory. Not part of a release build:
        // the whole section compiles away unless the research flag is set.
        //
        // Steps 1 and 2 aim at a SAVED LOCATION rather than at the player.
        // The coordinates of a saved spot are exact and already known, and the
        // player never has to move - so every stale copy of the player's own
        // position fails the second pass and drops out. Step 3 can then use
        // the live player position as a third independent filter.
        {
            static bool s_markerUseY = false;
            ui::Toggle("Use Y filter (X/Y/Z search)",
                       &s_markerUseY,
                       "Much sharper, but only if the marker stores a height. "
                       "If searches return 0, turn this off.");

            static float s_markerTolerance = 2.0f;
            if (ui::FloatOption("Search tolerance", &s_markerTolerance,
                                0.5f, 50.0f, 0.5f, 5.0f, "%.1f",
                                "World units. Tighter = fewer false positives, but "
                                "may miss the pin if the map snapped it away."))
            {
                game::Teleport::SetMarkerTolerance(s_markerTolerance);
            }

            char  n0[48] = {}, n1[48] = {};
            float x0 = 0, y0 = 0, z0 = 0, x1 = 0, y1 = 0, z1 = 0;
            const bool haveA = game::Teleport::GetBookmark(0, n0, sizeof(n0), &x0, &y0, &z0);
            const bool haveB = game::Teleport::GetBookmark(1, n1, sizeof(n1), &x1, &y1, &z1);

            char lbl[96], dsc[240];

            if (haveA)
            {
                snprintf(lbl, sizeof(lbl), "Search - Step 1 (%s)", n0);
                snprintf(dsc, sizeof(dsc),
                         "Set your destination to %s on the map, close the map, then "
                         "press this. Records every address holding X %.0f Y %.0f Z %.0f.",
                         n0, x0, y0, z0);
            }
            else
            {
                snprintf(lbl, sizeof(lbl), "Search - Step 1 (needs a saved spot)");
                snprintf(dsc, sizeof(dsc), "Save two spots first, in two different places.");
            }
            if (ui::Option(lbl, dsc) && haveA)
            {
                const int found = game::Teleport::MarkerSearchStep1(x0, y0, z0, s_markerUseY);
                ui::Toast("Step 1: %d candidate(s)", found);
            }

            if (haveB)
            {
                snprintf(lbl, sizeof(lbl), "Search - Step 2 (%s)", n1);
                snprintf(dsc, sizeof(dsc),
                         "Now move your destination to %s and press this. Keeps only "
                         "the addresses that followed it to X %.0f Y %.0f Z %.0f.",
                         n1, x1, y1, z1);
            }
            else
            {
                snprintf(lbl, sizeof(lbl), "Search - Step 2 (needs a second saved spot)");
                snprintf(dsc, sizeof(dsc),
                         "Step 2 needs a second saved spot, well away from the first.");
            }
            if (ui::Option(lbl, dsc) && haveB)
            {
                const int left = game::Teleport::MarkerSearchStep2(x1, y1, z1, s_markerUseY);
                ui::Toast("Step 2: %d left - see the log", left);
            }

            // Step 3: use the live player position as the third reference.
            // The user places the marker exactly where they are standing and
            // presses this to filter the survivors one more time.
            const float px = x + originX;
            const float py = y + originY;
            const float pz = z + originZ;
            const bool havePos = inWorld;
            snprintf(lbl, sizeof(lbl), "Search - Step 3 (current position)");
            snprintf(dsc, sizeof(dsc),
                     havePos
                         ? "Place the marker where you stand now, then press this. "
                           "Filters survivors against X %.0f Y %.0f Z %.0f."
                         : "Step 3 needs a live player position - load into the world first.",
                     px, py, pz);
            if (ui::Option(lbl, dsc) && havePos)
            {
                const int left = game::Teleport::MarkerSearchStep3(px, py, pz, s_markerUseY);
                ui::Toast("Step 3: %d left - see the log", left);
            }
        }
#endif

        ui::End();
    }
    static void RenderTravel()
    {
        ui::Begin();

        float x = 0.0f, y = 0.0f, z = 0.0f;
        if (game::Teleport::GetLastPosition(&x, &y, &z))
        {
            char buf[96];
            snprintf(buf, sizeof(buf), "X %.2f  Y %.2f  Z %.2f", x, y, z);
            if (ui::Option(buf, "Copy these coordinates to the clipboard."))
            {
                if (game::Teleport::CopyPositionToClipboard())
                    ui::Toast("Coordinates copied to clipboard");
            }
        }
        else
        {
            ui::Option("No position yet", "Load into the world and take a step first.");
        }

        {
            const size_t saved = game::Teleport::BookmarkCount();
            char lbl[64];
            if (saved > 0) snprintf(lbl, sizeof(lbl), "Saved Locations  (%zu)", saved);
            else           snprintf(lbl, sizeof(lbl), "Saved Locations");
            ui::Submenu(lbl, "savedloc",
                        "Keep any spot in the world and travel back to it later.");
        }

        // Teleport to the destination currently set on the world map or by a
        // quest objective. The hook captures the game's own destination vec3
        // when it is copied into the marker manager, so this always lands where
        // the map says to go.
        {
            float dx = 0, dy = 0, dz = 0;
            float px = 0, py = 0, pz = 0;
            const bool haveDest = game::Teleport::GetDestinationPosition(&dx, &dy, &dz);
            const bool havePos  = game::Teleport::GetLastPosition(&px, &py, &pz);
            char lbl[96];
            if (haveDest)
                snprintf(lbl, sizeof(lbl), "Teleport to Destination  (%.0f, %.0f, %.0f)", dx, dy, dz);
            else
                snprintf(lbl, sizeof(lbl), "Teleport to Destination");

            if (ui::Option(lbl,
                           haveDest
                               ? "Warp to the destination pin/objective on the map."
                               : "Set a destination on the map first."))
            {
                if (!havePos)
                {
                    ui::Toast("Load into the world first");
                }
                else if (!haveDest)
                {
                    ui::Toast("Set a destination on the map first");
                }
                else
                {
                    if (game::Teleport::WarpToDestination())
                        ui::Toast("Warping to destination");
                    else
                        ui::Toast("Another warp is still active");
                }
            }
        }

        // The game's own fast-travel network: every map gimmick (fast-travel
        // points, ores, chests, shops, bells, dungeons...), grouped by category.
        if (ui::Submenu("Fast Travel", "ftcats",
                        "Warp to any location on the map."))
        {
            game::Teleport::LoadCatalog();
        }

        ui::End();
    }

    // Shared state for the fast-travel category / node browser.
    static size_t s_ftCat        = 0;
    static char   s_ftFilter[48] = "";

    // Draws a `ui::Search` filter box, then `renderRow(i)` for i in
    // [0, total) - each call fetches + filter-tests + draws row i,
    // returning whether it passed the filter. Draws `noMatchDesc` under a
    // "No matches" row if every row was filtered out. Shared by every
    // long, searchable list menu (fast-travel nodes, inventory items) so
    // the search/counter/footer plumbing lives in one place.
    template <typename RenderRow>
    static void RenderFilteredList(size_t total, char* filterBuf, size_t filterCap,
                                   const char* searchDesc, const char* noMatchDesc,
                                   RenderRow renderRow)
    {
        ui::Search(filterBuf, filterCap, searchDesc);

        size_t shown = 0;
        for (size_t i = 0; i < total; ++i)
            if (renderRow(i))
                ++shown;
        if (shown == 0)
            ui::Option("No matches", noMatchDesc);
    }

    // Renders one `ui::Submenu` row per category (built into `label` by
    // `renderLabel(i, label, cap)`), pushing into `targetMenu`. Clears
    // `filterBuf` and resets `targetMenu`'s scroll/selection when the
    // selected category changes - a fresh list makes a leftover
    // filter/selection point at unrelated rows - then updates *currentCat
    // and calls onSelect(i). Shared by the fast-travel and inventory
    // category lists.
    // `iconFor` returns the category's game icon sprite name, or nullptr for a
    // list that has none (the fast-travel one).
    template <typename Index, typename RenderLabel, typename IconFor, typename OnSelect>
    static void RenderCategoryList(Index count, const char* targetMenu, const char* desc,
                                   Index* currentCat, char* filterBuf,
                                   RenderLabel renderLabel, IconFor iconFor, OnSelect onSelect)
    {
        for (Index i = 0; i < count; ++i)
        {
            char label[144];
            if (!renderLabel(i, label, sizeof(label))) continue;
            if (ui::SubmenuItem(label, iconFor(i), targetMenu, desc))
            {
                if (*currentCat != i)
                {
                    filterBuf[0] = 0;
                    ui::ResetMenu(targetMenu);
                }
                *currentCat = i;
                onSelect(i);
            }
        }
    }

    static void RenderFastTravelCats()
    {
        ui::Begin();
        ui::ListJump();

        if (!game::Teleport::LoadCatalog())
        {
            ui::Option("Building destination list...",
                       "Loading - if this persists, load into the world first.");
            ui::End();
            return;
        }

        const size_t n = game::Teleport::CategoryCount();
        RenderCategoryList(n, "ftnodes", "Browse and warp to this category's locations.",
                           &s_ftCat, s_ftFilter,
                           [](size_t i, char* label, size_t cap)
                           {
                               const char* name = nullptr;
                               size_t count = 0;
                               if (!game::Teleport::GetCategory(i, &name, &count)) return false;
                               snprintf(label, cap, "%s  (%zu)", name, count);
                               return true;
                           },
                           [](size_t) -> const char* { return nullptr; },
                           [](size_t i) { game::Teleport::EnsureCategoryNodes(i); });

        ui::End();
    }

    static void RenderFastTravelNodes()
    {
        ui::Begin();
        ui::ListJump();

        game::Teleport::EnsureCategoryNodes(s_ftCat);
        const size_t total = game::Teleport::NodeCount(s_ftCat);
        if (total == 0)
        {
            ui::Option("No locations", "Nothing to warp to in this category.");
            ui::End();
            return;
        }

        // One continuous list: the framework scrolls it, the breadcrumb bar
        // shows "row / total", Left/Right and PgUp/PgDn jump a screen at a
        // time, and the search row filters it live.
        RenderFilteredList(total, s_ftFilter, sizeof(s_ftFilter),
            "Search this list by name.",
            "No locations match this search.",
            [&](size_t i)
            {
                const char* label = nullptr;
                float nx = 0.0f, ny = 0.0f, nz = 0.0f;
                if (!game::Teleport::GetNode(s_ftCat, i, &label, &nx, &ny, &nz)) return false;
                if (s_ftFilter[0] && !ContainsNoCase(label, s_ftFilter)) return false;

                char desc[128];
                snprintf(desc, sizeof(desc), "Fast travel to %s at %.0f, %.0f, %.0f.",
                         label, nx, ny, nz);

                if (ui::Option(label, desc))
                {
                    if (game::Teleport::TravelToNode(s_ftCat, i))
                        ui::Toast("Warping to %s", label);
                }
                return true;
            });

        ui::End();
    }

    // --- Inventory -----------------------------------------------------------
    // Storage -> category -> item. The storage level is the game's own: your
    // pack, Private Storage, the Wardrobe and the Bank are separate storages
    // that all hang off the one holder, so without this level a stack in the
    // Bank and one in your pack are indistinguishable rows.
    //
    // Quantities are edited on the item's own row (ui::ItemRow) rather than in
    // a popup, so the list you browse is the list you edit.
    static int  s_invStore       = 0;
    static int  s_invCat         = 0;
    // Two separate filters, because they do different jobs: the storage page
    // searches EVERY category at once (find an item without knowing which
    // category the game files it under), the category page filters just its own
    // list. Sharing one buffer would carry a storage-wide search into a
    // category that has none of the matches.
    static char s_invFind[48]    = "";
    static char s_invFilter[48]  = "";
    // Add Item mirrors all of that, over the game's catalog instead of what you
    // own: its own category index, and the same two-filter split for the same
    // reason (s_invAddFind searches every category at once, s_invAddFilter just
    // the open one). The amount to add is not here - it lives on the item row
    // itself (ui::ItemAddRow), exactly like a quantity does in the Editor.
    static int  s_invAddCat        = 0;
    static char s_invAddFind[48]   = "";
    static char s_invAddFilter[48] = "";
    // Last storage the user actually entered, so re-entering the same one keeps
    // your place in its category list while switching storages starts fresh.
    static int  s_invStorePrev   = -1;
    // The amount Set All writes. Shared by every category rather than kept per
    // one: it is a number you are about to type anyway, and "999 in this
    // category, 12 in that one" is state nobody asked to keep.
    static int  s_invSetAllQty   = 999;
    // The amount Add All queues for each item, the Add Item mirror of the above.
    // Its own default (a modest count, since this is one press per whole
    // category, not per item) and, like Set All, shared across every category.
    static int  s_invAddAllQty   = 5;

    // Identity of one item row for ui::ItemRow, which needs to notice when a
    // row's item changes under an in-progress edit. Position alone is not
    // enough (the item at an index changes as stacks come and go) and type
    // alone is not either (a storage can hold two stacks of one item), so it
    // takes both. The +1 on `st` keeps the key non-zero, which ItemRow reads as
    // "no identity".
    static unsigned long long ItemKey(int st, int cat, int idx, uint16_t typeId)
    {
        return (static_cast<unsigned long long>(st + 1) & 0xFF) << 56 |
               (static_cast<unsigned long long>(cat + 1) & 0xFF) << 48 |
               (static_cast<unsigned long long>(idx) & 0xFFFF)   << 16 |
               static_cast<unsigned long long>(typeId);
    }

    // The same identity for a CATALOG row, in its own space: `st` there is a
    // storage index and real storages number under twenty, so 0x7F can never
    // collide with one. Catalog and inventory rows never share a page, but the
    // widgets that key off these are shared, and a key meaning two things is
    // the kind of bug that shows up once as an inexplicable glitch.
    static unsigned long long CatalogKey(int cat, int idx, uint16_t typeId)
    {
        return ItemKey(0x7F - 1, cat, idx, typeId);
    }

    // Whether item (st, cat, idx) is one the list would show under `filter`,
    // handing back its info when it is. RenderItemRow decides exactly this, but
    // Set All has to know the answer BEFORE the list is drawn (it says how many
    // items it is about to hit), so the rule lives here where both can read it
    // rather than being written out twice and drifting apart.
    static bool ItemShown(int st, int cat, int idx, const char* filter,
                          game::Inventory::ItemInfo* out)
    {
        game::Inventory::ItemInfo it{};
        if (!game::Inventory::GetItemInfo(st, cat, idx, &it)) return false;

        // Match the engine key and the raw type id as well as the display name.
        // An item whose definition does not resolve is listed as "Item #1234"
        // with no key text, so searching for what it is CALLED can never find
        // it - which is exactly when someone is trying to find it, because that
        // is the item they cannot use and want gone. Searching its number now
        // works, and the row shows that number for anything unnamed.
        if (filter && filter[0])
        {
            char idText[16];
            snprintf(idText, sizeof(idText), "%u", it.typeId);
            const bool hit = ContainsNoCase(it.name, filter) ||
                             (it.key && it.key[0] && ContainsNoCase(it.key, filter)) ||
                             ContainsNoCase(idText, filter);
            if (!hit) return false;
        }
        if (out) *out = it;
        return true;
    }

    // Draws item `idx` of (st, cat) as an editable row and applies whatever was
    // done to it. Returns false when the row does not exist or the filter
    // rejected it, so callers can count what they actually showed. `showCat`
    // names the item's category in the description - for the storage-wide
    // search, where the rows come from all over and otherwise would not say.
    // `locked` is the page's, not the row's: EditsPersist() reads live memory,
    // and a storage-wide search runs this for every item in the storage.
    static bool RenderItemRow(int st, int cat, int idx, const char* filter,
                              bool showCat, bool locked)
    {
        game::Inventory::ItemInfo it{};
        if (!ItemShown(st, cat, idx, filter, &it)) return false;

        char desc[224];
        if (locked)
            snprintf(desc, sizeof(desc),
                     "Editing is locked until your save finishes loading.");
        else if (showCat)
            snprintf(desc, sizeof(desc), "%s, in %s.",
                     it.name, game::Inventory::CategoryName(st, cat));
        else if (strncmp(it.name, "Item #", 6) == 0)
            // Unnamed means the game has no definition for it - the state a
            // spawned item ends up in when it never reached the authoritative
            // side. Say so, and say what to search for to find it again.
            snprintf(desc, sizeof(desc),
                     "The game has no definition for this one (id %u) - it cannot be used. "
                     "Press Del twice to remove it.", it.typeId);
        else
            snprintf(desc, sizeof(desc), "Change how many you have, or remove it.");

        long long nq = 0;
        const ui::ItemEdit e = ui::ItemRow(it.name, it.icon, it.qty,
                                           ItemKey(st, cat, idx, it.typeId),
                                           locked, &nq, desc);
        if (e == ui::ItemEdit::SetQty)
        {
            // No refresh needed: SetQuantity updates the snapshot too, so a held
            // Left/Right steps off the value it just wrote instead of stalling
            // on a stale one until the refresh throttle lets go.
            game::Inventory::SetQuantity(st, cat, idx, nq);
        }
        else if (e == ui::ItemEdit::Remove)
        {
            if (game::Inventory::RemoveItem(st, cat, idx))
            {
                ui::Toast("Removed %s", it.name);
                game::Inventory::ForceRefresh(); // drop the row now, not in 120ms
            }
        }
        return true;
    }

    // The Inventory tab's own page: the entry point into the item browser/
    // editor (moved to its own submenu so this page has room for inventory-
    // wide settings that aren't about any one item), plus two overrides that
    // write straight into the game's data tables - Slot Size re-stamps every
    // storage's slot count, Max Stack Size re-stamps every item's stack cap.
    // Both are all-at-once, not per-item/per-storage, on purpose: the ask was
    // "one number for everything", not a per-row editor.
    static void RenderInventoryHome()
    {
        State& st = State::Get();
        ui::Begin();

        ui::Submenu("Add Item", "invadd", "Add any item in the game to your inventory.");
        ui::Submenu("Camp Resources & Currencies", "invcamp",
                    "Quickly add camp funds, food, timber, stone, weapons, and currencies.");
        ui::Submenu("Quest & Special Items", "invspecial",
                    "Bounty notices, documents, quest memories, recipes and relics.");
        ui::Submenu("Item Editor", "invedit", "Browse and edit what you're carrying.");
        {
            // The count is the whole reason to look in here, so it goes on the
            // row rather than behind it. Submenu() strips the trailing count
            // when it builds the breadcrumb title.
            const int lost = game::Inventory::LostCount();
            char lbl[64];
            if (lost > 0) snprintf(lbl, sizeof(lbl), "Restore Items  (%d)", lost);
            else          snprintf(lbl, sizeof(lbl), "Restore Items");
            ui::Submenu(lbl, "invlost",
                        "Get back anything you sold, destroyed or used up.");
        }

        // No money feature is exposed, and both routes were tried and measured.
        //
        // 1. Silver pouches. `Silver_Pack` resolves and the item lands cleanly
        //    in both realms - but the pouch cannot be used in game at all. The
        //    mod this idea came from says so in its own UI: it CONVERTS pouches
        //    itself rather than letting the player use them.
        // 2. The wallet. It is not hidden - the Item Editor shows it as "Copper"
        //    under Currency, reading 4118718 against a HUD of 41187.18, an exact
        //    match. But raising it to 4120187 left the HUD unmoved: that item is
        //    a passive mirror, and the figure the game spends from lives
        //    somewhere this mod has not reached.
        //
        // A button that adds an unusable item, or edits a number nothing reads,
        // is worse than no button - so neither ships until the real wallet is
        // found. Inventory::FindCurrency / ProbeCurrency are kept for that.
        // NOTE: a "Find My Silver" row used to sit here - a diagnostic that
        // asked the player to type their HUD balance so the log could report
        // where that number lived. It never found it (two passes, closest
        // result off by ~9200), and money is now handled by adding the game's
        // own silver pouches instead, which needs no address at all. A row
        // that writes only to a log has no business next to real features in a
        // public build. Inventory::FindCurrency and ProbeCurrency are kept for
        // when direct balance editing is worth another attempt.

        bool changed = false;
        if (ui::ToggleInt("Slot Size", &st.invSlotSize, &st.invSlotSizeVal,
                          1, 9999, 10, 2000,
                          "Sets every storage's slot count to this number."))
            changed = true;
        if (ui::ToggleInt("Max Stack Size", &st.invStackSize, &st.invStackSizeVal,
                          1, 999999999, 1000, 999999,
                          "Sets every item's max stack size to this number."))
            changed = true;

        if (changed && st.autoSave)
            Settings::Save();

        ui::End();
    }

    static void RenderInventoryEditor()
    {
        ui::Begin();

        if (!game::Inventory::Ready())
        {
            ui::Option("Loading inventory...",
                       "Reading your items - if this persists, open your in-game "
                       "inventory once and come back here.");
            ui::End();
            return;
        }

        game::Inventory::Refresh();

        // One row per storage that actually holds something. Listed first so
        // the row selected on arrival is a storage - the thing this tab is for -
        // and not the housekeeping below it.
        const int n = game::Inventory::StorageCount();
        RenderCategoryList(n, "invstore", "Browse and edit what is kept in this storage.",
                           &s_invStore, s_invFind,
                           [](int s, char* label, size_t cap)
                           {
                               const int cnt = game::Inventory::StorageItemCount(s);
                               if (cnt == 0) return false;
                               snprintf(label, cap, "%s  (%d)", game::Inventory::StorageName(s), cnt);
                               return true;
                           },
                           [](int) -> const char* { return nullptr; }, // storages carry no icon
                           [](int s)
                           {
                               if (s_invStorePrev == s) return;
                               s_invStorePrev = s;
                               s_invCat = 0;
                               ui::ResetMenu("invcat");
                           });

        if (ui::Option("Refresh", "Reloads your inventory from the game."))
        {
            game::Inventory::ForceRefresh();
            ui::Toast("Inventory refreshed");
        }

        ui::End();
    }

    // One storage: its categories, or - once the search box has anything in it -
    // the matching items from every one of them, editable right here. That is
    // the shortcut for the common case, where you know the item's name but not
    // the category the game keeps it in.
    static void RenderInventoryStorage()
    {
        ui::Begin(game::Inventory::StorageName(s_invStore));

        game::Inventory::Refresh();

        const int n = game::Inventory::CategoryCount(s_invStore);
        if (n == 0)
        {
            ui::Option("Empty", "Nothing in this storage right now.");
            ui::End();
            return;
        }

        // Left/Right belong to the item rows when searching; page jumps would
        // eat them (ListJump consumes both), so they are only mapped while this
        // page is a plain category list.
        if (!s_invFind[0])
            ui::ListJump();

        ui::Search(s_invFind, sizeof(s_invFind),
                   "Find an item anywhere in this storage, whatever category it is in.");

        if (!s_invFind[0])
        {
            RenderCategoryList(n, "invcat", "Browse and edit items in this category.",
                               &s_invCat, s_invFilter,
                               [](int c, char* label, size_t cap)
                               {
                                   const int cnt = game::Inventory::ItemCount(s_invStore, c);
                                   if (cnt == 0) return false;
                                   snprintf(label, cap, "%s  (%d)",
                                            game::Inventory::CategoryName(s_invStore, c), cnt);
                                   return true;
                               },
                               [](int c) { return game::Inventory::CategoryIcon(s_invStore, c); },
                               [](int) {});
            ui::End();
            return;
        }

        const bool locked = !game::Inventory::EditsPersist();
        int shown = 0;
        for (int c = 0; c < n; ++c)
        {
            const int items = game::Inventory::ItemCount(s_invStore, c);
            for (int i = 0; i < items; ++i)
                if (RenderItemRow(s_invStore, c, i, s_invFind, /*showCat=*/true, locked))
                    ++shown;
        }
        if (shown == 0)
            ui::Option("No matches", "Nothing in this storage is called that.");

        ui::End();
    }

    // Set All: one quantity onto every item the category page is showing, so a
    // category of forty arrows takes one action instead of forty. `shown` is
    // how many rows the list below will draw - the filter narrows what Set All
    // touches, because the list you are looking at is the list it edits, the
    // same promise the per-item rows make.
    //
    // The amount and the action are one row (ui::IntAction, which exists for
    // this): typing an exact amount and firing are still two separate presses,
    // so nothing here writes to forty items on the press that starts an edit -
    // that just no longer costs a second row and a trip between the two to say
    // one number. The description does not name the amount any more, because
    // the amount is now sitting on the same row it describes.
    static void RenderSetAll(int shown, bool locked)
    {
        char desc[224];
        if (locked)
            snprintf(desc, sizeof(desc),
                     "Editing is locked until your save finishes loading.");
        else if (shown == 0)
            snprintf(desc, sizeof(desc), "Nothing here to set.");
        else if (s_invFilter[0])
            snprintf(desc, sizeof(desc), "Sets the %d matching item%s below to this amount.",
                     shown, shown == 1 ? "" : "s");
        else
            snprintf(desc, sizeof(desc), "Sets every item in %s to this amount - all %d of them.",
                     game::Inventory::CategoryName(s_invStore, s_invCat), shown);

        // A locked or empty page still shows the row and its amount, it just
        // has nothing to write: an inert row that says why beats one that
        // vanishes and takes the explanation with it.
        const bool inert = locked || shown == 0;
        if (!ui::IntAction("Set All", &s_invSetAllQty, 1, 999999999, 1, 999, desc) ||
            inert)
            return;

        // Indices stay put: SetQuantity edits the stack in place, it never adds
        // or drops one, so the list is not reordered under this loop.
        const int total = game::Inventory::ItemCount(s_invStore, s_invCat);
        int done = 0;
        for (int i = 0; i < total; ++i)
            if (ItemShown(s_invStore, s_invCat, i, s_invFilter, nullptr) &&
                game::Inventory::SetQuantity(s_invStore, s_invCat, i, s_invSetAllQty))
                ++done;

        ui::Toast("Set %d item%s to x%d", done, done == 1 ? "" : "s", s_invSetAllQty);
    }

    static void RenderInventoryCat()
    {
        ui::Begin(game::Inventory::CategoryName(s_invStore, s_invCat));

        game::Inventory::Refresh();
        const int total = game::Inventory::ItemCount(s_invStore, s_invCat);
        if (total == 0)
        {
            ui::Option("Empty", "Nothing in this category right now.");
            ui::End();
            return;
        }

        // No ListJump here at all: every row below is an item row that wants
        // Left/Right for its amount. PgUp/PgDn/Home/End still page the list.
        ui::Search(s_invFilter, sizeof(s_invFilter), "Narrow this category down by name.");

        const bool locked = !game::Inventory::EditsPersist();

        // Exactly the rows the list below will draw, counted before Set All so
        // it can name the number, and reused afterwards for "No matches".
        int shown = 0;
        for (int i = 0; i < total; ++i)
            if (ItemShown(s_invStore, s_invCat, i, s_invFilter, nullptr))
                ++shown;

        RenderSetAll(shown, locked);

        for (int i = 0; i < total; ++i)
            RenderItemRow(s_invStore, s_invCat, i, s_invFilter, /*showCat=*/false, locked);
        if (shown == 0)
            ui::Option("No matches", "Nothing in this category is called that.");

        ui::End();
    }

    static void RenderInventoryCamp()
    {
        ui::Begin("Camp Resources & Currencies");

        if (ui::Option("Add Max Camp Resources (+99,999 All)",
                       "Adds 99,999 of Camp Funds, Food, Timber, Stone, and Weapons to your camp storage."))
        {
            if (game::Inventory::AddCampResources(99999))
                ui::Toast("Queued Max Camp Resources (+99,999 each)");
            else
                ui::Toast("%s", game::Inventory::LastAddFailure());
        }

        if (ui::Option("Add 1,000,000 Copper", "Adds 1,000,000 Copper currency to your inventory."))
        {
            if (game::Inventory::AddItem(1, 1000000))
                ui::Toast("Added 1,000,000 Copper");
            else
                ui::Toast("%s", game::Inventory::LastAddFailure());
        }

        if (ui::Option("Add 1,000 Gold Bars", "Adds 1,000 Gold Bars to your inventory."))
        {
            if (game::Inventory::AddItem(53, 1000))
                ui::Toast("Added 1,000 Gold Bars");
            else
                ui::Toast("%s", game::Inventory::LastAddFailure());
        }

        if (ui::Option("Add 1,000 Pearls", "Adds 1,000 Pearls to your inventory."))
        {
            if (game::Inventory::AddItem(2, 1000))
                ui::Toast("Added 1,000 Pearls");
            else
                ui::Toast("%s", game::Inventory::LastAddFailure());
        }

        if (ui::Option("Add 100 Overflowing Silver Pouches", "Adds 100 Overflowing Silver Pouches to your inventory."))
        {
            if (game::Inventory::AddItem(105, 100))
                ui::Toast("Added 100 Overflowing Silver Pouches");
            else
                ui::Toast("%s", game::Inventory::LastAddFailure());
        }

        ui::End();
    }

    // Add Item: category -> item, the same walk as the Editor. Deliberately so -
    // it is the same information out of the same game tables, and a second,
    // differently-shaped way to look at your items would just be something else
    // to learn. The only level the Editor has that this cannot is storage: a
    // catalog item is not in one yet, and when added it picks its own.

    // The add itself runs on the game thread a frame or so after the click, so
    // its result is reported wherever we are when it lands, not at the click.
    static void ReportPendingAdd()
    {
        static game::Inventory::AddState s_lastAdd = game::Inventory::AddState::Idle;
        const game::Inventory::AddState now = game::Inventory::AddStatus();
        if (now == s_lastAdd) return;
        if (now == game::Inventory::AddState::Added)
        {
            ui::Toast("Item added");
            game::Inventory::ForceRefresh(); // show it in the Editor immediately
        }
        else if (now == game::Inventory::AddState::Failed)
        {
            // Say what actually went wrong. "See the log" is useless advice
            // while someone is standing in the game with the menu open.
            const char* why = game::Inventory::LastAddFailure();
            ui::Toast((why && *why) ? why : "Could not add that item - see the log");
        }
        s_lastAdd = now;
    }

    // The Add All companion to ReportPendingAdd: a bulk add drains over a couple
    // of seconds on the game thread, so its result lands here whenever we are
    // when the queue empties. Latches on the active->idle edge, once.
    static void ReportBulkAdd()
    {
        static bool s_wasActive = false;
        const game::Inventory::BulkAdd b = game::Inventory::BulkAddStatus();
        if (s_wasActive && !b.active)
        {
            if (b.failed == 0)
                ui::Toast("Added %d item%s", b.added, b.added == 1 ? "" : "s");
            else
                ui::Toast("Added %d, %d could not be added - see the log", b.added, b.failed);
            game::Inventory::ForceRefresh(); // show them in the Editor immediately
        }
        s_wasActive = b.active;
    }

    // One catalog item as an addable row. Mirrors RenderItemRow: same shape,
    // same filter test, returns whether it actually showed. `showCat` names the
    // item's category for the catalog-wide search, where rows come from all over
    // and would otherwise not say where they belong.
    static bool RenderAddRow(int cat, int idx, const char* filter, bool showCat, bool locked)
    {
        game::Inventory::ItemInfo it{};
        if (!game::Inventory::GetCatalogItem(cat, idx, &it)) return false;
        if (filter && filter[0] && !ContainsNoCase(it.name, filter)) return false;

        char desc[224];
        if (locked)
            snprintf(desc, sizeof(desc), "Adding is locked until your save finishes loading.");
        else if (showCat)
            snprintf(desc, sizeof(desc), "%s, in %s.",
                     it.name, game::Inventory::CatalogCategoryName(cat));
        else
            snprintf(desc, sizeof(desc), "Set how many, then add it.");

        // The catalog is static, so an item's identity here is just where it is.
        const unsigned long long key = CatalogKey(cat, idx, it.typeId);
        const long long n = ui::ItemAddRow(it.name, it.icon, key, locked, desc);
        if (n > 0)
        {
            if (game::Inventory::AddItem(it.typeId, n))
                ui::Toast("Adding %lld x %s...", n, it.name);
            else
                ui::Toast("Could not add %s", it.name);
        }
        return true;
    }

    // Lost & Sold. The list is the feature; the restore is one press. Rows are
    // newest first, so the thing you just sold by mistake is the top row.
    static void RenderInventoryLost()
    {
        ui::Begin();
        ReportPendingAdd();
        ReportBulkAdd();

        const int n = game::Inventory::LostCount();
        if (n == 0)
        {
            ui::Option("Nothing lost yet",
                       "Anything you sell, destroy or use up from here on will be listed "
                       "here so you can get it back.");
            ui::End();
            return;
        }

        if (ui::Option(">> Restore All Lost & Sold Items <<",
                       "Puts every item on this list back into your inventory."))
        {
            if (game::Inventory::RestoreAllLost())
                ui::Toast("Restoring %d item%s", n, n == 1 ? "" : "s");
            else
                ui::Toast("Busy - try again in a moment");
        }

        if (ui::Option("Clear List",
                       "Forgets everything below without restoring it."))
        {
            game::Inventory::ClearLost();
            ui::Toast("List cleared");
        }

        for (int i = 0; i < n; ++i)
        {
            uint16_t tid = 0;
            int64_t  qty = 0;
            char     name[96] = {};
            if (!game::Inventory::GetLost(i, &tid, &qty, name, sizeof(name))) continue;

            char label[160];
            snprintf(label, sizeof(label), "%s  x%lld", name, static_cast<long long>(qty));

            char icon[64] = {};
            const bool haveIcon = game::Inventory::IconForTypeId(tid, icon, sizeof(icon));
            const bool hit = haveIcon
                ? ui::OptionItem(label, icon, "Put this one back in your inventory.")
                : ui::Option(label, "Put this one back in your inventory.");
            if (hit)
            {
                if (game::Inventory::RestoreLost(i))
                {
                    ui::Toast("Restoring %s", name);
                    break;      // the list just shifted under us
                }
                ui::Toast("Busy - try again in a moment");
            }
        }

        ui::End();
    }
    // Quest & Special Items.
    //
    // Not a second catalog - the same one, narrowed. Every row here is already
    // reachable from Add Item; the problem is that bounty notices, quest
    // memories and lore documents sit scattered across thousands of entries in
    // groups nobody thinks to open. This collects the game's own quest/lore
    // groups onto one page and routes into the ordinary category view, so the
    // add path, the search and the amount controls are all the ones that
    // already work.
    static void RenderInventorySpecial()
    {
        ui::Begin();
        ui::ListJump();

        const int n = game::Inventory::CatalogCategoryCount(); // builds it on first call
        if (n == 0)
        {
            ui::Option("Catalog unavailable",
                       "The game's item table did not resolve, so there is nothing to list.");
            ui::End();
            return;
        }

        int shown = 0;
        RenderCategoryList(n, "invaddcat", "Add any item from this category.",
                           &s_invAddCat, s_invAddFilter,
                           [&shown](int c, char* label, size_t cap)
                           {
                               if (!game::Inventory::IsSpecialCategory(c)) return false;
                               const int cnt = game::Inventory::CatalogItemCount(c);
                               if (cnt == 0) return false;
                               snprintf(label, cap, "%s  (%d)",
                                        game::Inventory::CatalogCategoryPlainName(c), cnt);
                               ++shown;
                               return true;
                           },
                           [](int c) { return game::Inventory::CatalogCategoryIcon(c); },
                           [](int) {});

        if (shown == 0)
            ui::Option("Nothing found",
                       "This game build has no quest or document categories the mod "
                       "recognises. Everything is still in Add Item.");

        ui::End();
    }
    static void RenderInventoryAdd()
    {
        ui::Begin();
        ReportPendingAdd();
        ReportBulkAdd();

        const int n = game::Inventory::CatalogCategoryCount(); // builds it on first call
        if (n == 0)
        {
            ui::Option("Catalog unavailable",
                       "The game's item table did not resolve, so there is nothing to add from.");
            ui::End();
            return;
        }

        // Same rule as the storage page: Left/Right belong to the item rows when
        // searching, and ListJump consumes both, so it is only mapped while this
        // page is a plain category list.
        if (!s_invAddFind[0])
            ui::ListJump();

        ui::Search(s_invAddFind, sizeof(s_invAddFind),
                   "Find any item in the game by name, whatever category it is in.");

        if (!s_invAddFind[0])
        {
            RenderCategoryList(n, "invaddcat", "Add any item from this category.",
                               &s_invAddCat, s_invAddFilter,
                               [](int c, char* label, size_t cap)
                               {
                                   const int cnt = game::Inventory::CatalogItemCount(c);
                                   if (cnt == 0) return false;
                                   snprintf(label, cap, "%s  (%d)",
                                            game::Inventory::CatalogCategoryName(c), cnt);
                                   return true;
                               },
                               [](int c) { return game::Inventory::CatalogCategoryIcon(c); },
                               [](int) {});
            ui::End();
            return;
        }

        const bool locked = !game::Inventory::EditsPersist();
        int shown = 0;
        for (int c = 0; c < n && shown < 200; ++c)
        {
            const int items = game::Inventory::CatalogItemCount(c);
            for (int i = 0; i < items && shown < 200; ++i)
                if (RenderAddRow(c, i, s_invAddFind, /*showCat=*/true, locked))
                    ++shown;
        }
        // The catalog is thousands of rows deep - far more than the storage-wide
        // search it mirrors ever sees - so a short search matches hundreds. Cap
        // the list rather than build a page nobody can get to the bottom of.
        if (shown == 0)
            ui::Option("No matches", "No item in the game is called that.");
        else if (shown >= 200)
            ui::Option("More matches...", "Only the first 200 are listed - keep typing to narrow it down.");

        ui::End();
    }

    // Add All: one amount of every catalog item the category page is showing,
    // queued in a single action - the Add Item mirror of the Editor's Set All.
    // `shown` is how many rows the list below will draw, so the filter narrows
    // what Add All touches too: the list you are looking at is the list it adds.
    // The adds run a few per frame on the game thread (see AddItemsBulk), so a
    // big category fills in over a second or two rather than hitching a frame.
    static void RenderAddAll(int cat, const char* filter, int shown, bool locked)
    {
        char desc[224];
        if (locked)
            snprintf(desc, sizeof(desc), "Adding is locked until your save finishes loading.");
        else if (shown == 0)
            snprintf(desc, sizeof(desc), "Nothing here to add.");
        else if (filter && filter[0])
            snprintf(desc, sizeof(desc), "Adds this many of each of the %d matching item%s below.",
                     shown, shown == 1 ? "" : "s");
        else
            snprintf(desc, sizeof(desc), "Adds this many of every item in %s - all %d of them.",
                     game::Inventory::CatalogCategoryName(cat), shown);

        // A locked or empty page still shows the row and its amount, it just has
        // nothing to queue - the same choice Set All makes.
        const bool inert = locked || shown == 0;
        if (!ui::IntAction("Add All", &s_invAddAllQty, 1, 999999999, 1, 5, desc) || inert)
            return;

        // Gather exactly the rows the list draws, then hand the batch off in one
        // call. AddItemsBulk re-checks each id, so a stale row is skipped, not a
        // failure.
        std::vector<uint16_t> ids;
        ids.reserve(static_cast<size_t>(shown));
        const int total = game::Inventory::CatalogItemCount(cat);
        for (int i = 0; i < total; ++i)
        {
            game::Inventory::ItemInfo it{};
            if (!game::Inventory::GetCatalogItem(cat, i, &it)) continue;
            if (filter && filter[0] && !ContainsNoCase(it.name, filter)) continue;
            ids.push_back(it.typeId);
        }
        if (ids.empty()) return;

        const int count = static_cast<int>(ids.size());
        if (game::Inventory::AddItemsBulk(ids.data(), count, s_invAddAllQty))
            ui::Toast("Adding %d of each - %d item%s...",
                      s_invAddAllQty, count, count == 1 ? "" : "s");
        else
            ui::Toast("%s", game::Inventory::LastAddFailure());
    }

    static void RenderInventoryAddCat()
    {
        ui::Begin(game::Inventory::CatalogCategoryName(s_invAddCat));
        ReportPendingAdd();
        ReportBulkAdd();

        const int total = game::Inventory::CatalogItemCount(s_invAddCat);
        if (total == 0)
        {
            ui::Option("Empty", "Nothing in this category.");
            ui::End();
            return;
        }

        // No ListJump: every row below (and Add All itself) wants Left/Right for
        // its amount.
        ui::Search(s_invAddFilter, sizeof(s_invAddFilter), "Narrow this category down by name.");

        const bool locked = !game::Inventory::EditsPersist();

        // Count what the filter will show first, so Add All can name the number -
        // the same order the Editor's Set All uses.
        int shown = 0;
        for (int i = 0; i < total; ++i)
        {
            game::Inventory::ItemInfo it{};
            if (!game::Inventory::GetCatalogItem(s_invAddCat, i, &it)) continue;
            if (s_invAddFilter[0] && !ContainsNoCase(it.name, s_invAddFilter)) continue;
            ++shown;
        }

        RenderAddAll(s_invAddCat, s_invAddFilter, shown, locked);

        for (int i = 0; i < total; ++i)
            RenderAddRow(s_invAddCat, i, s_invAddFilter, /*showCat=*/false, locked);
        if (shown == 0)
            ui::Option("No matches", "Nothing in this category is called that.");

        ui::End();
    }

    // Human-readable name for a Win32 virtual-key code (rebind row display).
    // The named cases cover keys GetKeyNameText renders poorly (or needs the
    // extended-key bit for); the fallback handles letters/digits/F-keys.
    static const char* KeyName(int vk)
    {
        switch (vk)
        {
        case 0:           return "None";
        case VK_INSERT:   return "Insert";
        case VK_DELETE:   return "Delete";
        case VK_HOME:     return "Home";
        case VK_END:      return "End";
        case VK_PRIOR:    return "Page Up";
        case VK_NEXT:     return "Page Down";
        case VK_UP:       return "Up Arrow";
        case VK_DOWN:     return "Down Arrow";
        case VK_LEFT:     return "Left Arrow";
        case VK_RIGHT:    return "Right Arrow";
        case VK_NUMLOCK:  return "Num Lock";
        case VK_SNAPSHOT: return "Print Screen";
        case VK_DIVIDE:   return "Num /";
        default: break;
        }
        static char buf[32];
        const UINT sc = MapVirtualKeyA(static_cast<UINT>(vk), MAPVK_VK_TO_VSC);
        if (sc && GetKeyNameTextA(static_cast<LONG>(sc << 16), buf, sizeof(buf)) > 0)
            return buf;
        snprintf(buf, sizeof(buf), "Key 0x%02X", vk);
        return buf;
    }

    // Human-readable name for an XInput button mask (plus the trigger
    // sentinel bits, State::kPadLTrigger/kPadRTrigger); a combo joins with
    // " + ".
    static const char* PadMaskName(unsigned int mask)
    {
        static const struct { unsigned int bit; const char* name; } kBtns[] = {
            { XINPUT_GAMEPAD_LEFT_SHOULDER,  "LB" },
            { XINPUT_GAMEPAD_RIGHT_SHOULDER, "RB" },
            { XINPUT_GAMEPAD_DPAD_UP,        "D-Pad Up" },
            { XINPUT_GAMEPAD_DPAD_DOWN,      "D-Pad Down" },
            { XINPUT_GAMEPAD_DPAD_LEFT,      "D-Pad Left" },
            { XINPUT_GAMEPAD_DPAD_RIGHT,     "D-Pad Right" },
            { XINPUT_GAMEPAD_A,              "A" },
            { XINPUT_GAMEPAD_B,              "B" },
            { XINPUT_GAMEPAD_X,              "X" },
            { XINPUT_GAMEPAD_Y,              "Y" },
            { XINPUT_GAMEPAD_START,          "Start" },
            { XINPUT_GAMEPAD_BACK,           "Back" },
            { XINPUT_GAMEPAD_LEFT_THUMB,     "LS" },
            { XINPUT_GAMEPAD_RIGHT_THUMB,    "RS" },
            { kPadLTrigger,                  "LT" },
            { kPadRTrigger,                  "RT" },
        };
        static char buf[96];
        buf[0] = 0;
        for (const auto& b : kBtns)
            if (mask & b.bit)
            {
                if (buf[0]) strncat(buf, " + ", sizeof(buf) - strlen(buf) - 1);
                strncat(buf, b.name, sizeof(buf) - strlen(buf) - 1);
            }
        return buf[0] ? buf : "None";
    }

    // Lowest virtual-key currently held (mouse buttons 0x01-0x06 skipped),
    // or 0 if none. Used to detect the key the user wants to bind.
    static int FirstKeyDown()
    {
        for (int vk = 0x08; vk <= 0xFE; ++vk)
            if (GetAsyncKeyState(vk) & 0x8000)
                return vk;
        return 0;
    }

    // Which bind is currently listening for a key/button press, or None. Only
    // one column of one row can capture at a time - starting a new one abandons
    // any other. The Key/Pad pair per action maps onto the two columns of a
    // single ui::BindRow.
    enum class BindTarget
    {
        None, MenuKey, MenuPad, FlyUpKey, FlyUpPad, FlyDownKey, FlyDownPad
    };
    static BindTarget s_capTarget = BindTarget::None;

    // One action drawn as a two-column keyboard | controller rebind row (see
    // ui::BindRow). Formats each column's current bind (or a "press..." prompt
    // for the column that owns the live capture), then turns the row's result
    // into a capture request or a per-column reset. `keyTarget` / `padTarget`
    // name this action's two capture slots; `def*` are the defaults a reset
    // restores (so a reset can never strand the menu with no way to reopen it).
    static void KeybindActionRow(const char* label, const char* desc, int* cursor,
                                 int* keyVk, unsigned int* padMask,
                                 int defKeyVk, unsigned int defPadMask,
                                 BindTarget keyTarget, BindTarget padTarget)
    {
        const bool capKey = (s_capTarget == keyTarget);
        const bool capPad = (s_capTarget == padTarget);

        char keyBuf[48], padBuf[64];
        snprintf(keyBuf, sizeof(keyBuf), "%s", capKey ? "press a key..." : KeyName(*keyVk));
        snprintf(padBuf, sizeof(padBuf), "%s", capPad ? "press a button..." : PadMaskName(*padMask));

        // While listening, the description spells out how to finish; otherwise
        // it's the action's own explanation.
        const char* rowDesc = desc;
        if      (capKey) rowDesc = "Press the key you want to bind, or Esc to cancel.";
        else if (capPad) rowDesc = "Press the button or combo you want to bind, or Esc to cancel.";

        const int capCol = capKey ? 0 : capPad ? 1 : -1;
        switch (ui::BindRow(label, cursor, keyBuf, padBuf, capCol, rowDesc))
        {
        case ui::BindEdit::RebindKey:
            s_capTarget = capKey ? BindTarget::None : keyTarget; // toggle listening
            break;
        case ui::BindEdit::RebindPad:
            s_capTarget = capPad ? BindTarget::None : padTarget;
            break;
        case ui::BindEdit::ResetKey:
            *keyVk = defKeyVk;
            if (capKey) s_capTarget = BindTarget::None;
            Settings::Save(); // binds persist regardless of Auto Save
            ui::Toast("%s keyboard bind reset to %s", label, KeyName(defKeyVk));
            break;
        case ui::BindEdit::ResetPad:
            *padMask = defPadMask;
            if (capPad) s_capTarget = BindTarget::None;
            Settings::Save();
            ui::Toast("%s controller bind reset to %s", label, PadMaskName(defPadMask));
            break;
        default:
            break;
        }
    }

    // Drives the "press a key / button to bind" capture for whichever row is
    // currently listening (s_capTarget). Kept out of RenderKeybinds' body so
    // its rows read cleanly. Returns true if a capture is (still) active, so
    // the caller keeps State::rebindCapture in sync.
    static bool DriveRebindCapture(State& st)
    {
        // Per-capture phase state.
        static bool         keyArmed = false, padArmed = false;
        static int          pendKey  = 0;
        static unsigned int padAccum = 0;
        static ULONGLONG    startMs  = 0;

        // A fresh capture request (including switching targets mid-listen)
        // resets the phase machine and starts the clock.
        static BindTarget wasTarget = BindTarget::None;
        const bool        capturing = s_capTarget != BindTarget::None;
        if (capturing && wasTarget != s_capTarget)
        {
            keyArmed = padArmed = false;
            pendKey = 0; padAccum = 0;
            startMs = GetTickCount64();
        }
        wasTarget = s_capTarget;
        if (!capturing)
            return false;

        // Resolve which field the active target writes and what the toast calls
        // it. Only the Free Flight pad binds accept the analog-trigger
        // sentinels - Menu Button is polled elsewhere (PollToggleCombo) purely
        // off the real wButtons mask, so a trigger could never fire it.
        int*          keyField       = nullptr;
        unsigned int* padField       = nullptr;
        bool          padTriggersOk  = false;
        const char*   label          = "";
        switch (s_capTarget)
        {
        case BindTarget::MenuKey:    keyField = &st.openKeyVk;      label = "Menu key";        break;
        case BindTarget::MenuPad:    padField = &st.openPadMask;    label = "Menu button";     break;
        case BindTarget::FlyUpKey:   keyField = &st.flyUpKeyVk;     label = "Fly Up key";      break;
        case BindTarget::FlyUpPad:   padField = &st.flyUpPadMask;   label = "Fly Up button";   padTriggersOk = true; break;
        case BindTarget::FlyDownKey: keyField = &st.flyDownKeyVk;   label = "Fly Down key";    break;
        case BindTarget::FlyDownPad: padField = &st.flyDownPadMask; label = "Fly Down button"; padTriggersOk = true; break;
        default: break;
        }

        const bool timedOut = GetTickCount64() - startMs > 6000; // never soft-lock
        const bool escDown  = (GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0;

        if (keyField)
        {
            if (timedOut)                     s_capTarget = BindTarget::None;   // give up, keep old bind
            else if (!keyArmed)               keyArmed = (FirstKeyDown() == 0); // let the activating press lift
            else if (pendKey == 0)
            {
                if (escDown) s_capTarget = BindTarget::None;                    // cancel
                else { const int vk = FirstKeyDown(); if (vk && vk != VK_ESCAPE) pendKey = vk; }
            }
            else if (!(GetAsyncKeyState(pendKey) & 0x8000))                     // commit once the key lifts
            {
                *keyField = pendKey;
                Settings::Save();                                               // binds persist regardless of Auto Save
                ui::Toast("%s set to %s", label, KeyName(pendKey));
                s_capTarget = BindTarget::None;
            }
        }
        else if (padField)
        {
            const unsigned int btns = padTriggersOk ? ui::PadButtonsWithTriggers()
                                                      : static_cast<unsigned int>(ui::PadButtons());
            if (timedOut || escDown)          s_capTarget = BindTarget::None;
            else if (!padArmed)               padArmed = (btns == 0);  // let the activating A lift
            else if (btns != 0)               padAccum |= btns;        // accumulate the held combo
            else if (padAccum != 0)                                    // commit on release
            {
                *padField = padAccum;
                Settings::Save();
                ui::Toast("%s set to %s", label, PadMaskName(padAccum));
                s_capTarget = BindTarget::None;
            }
        }

        return s_capTarget != BindTarget::None;
    }

    static void RenderKeybinds()
    {
        State&      st  = State::Get();
        const State def;                 // source of every reset-to-default value
        ui::Begin();

        // Each action is one row with its keyboard bind and controller bind side
        // by side under the two column titles. Left/Right pick the column, Enter
        // rebinds it (then press the key/button - Esc cancels), Del resets it.
        // Every bind here persists in Trinity.ini regardless of Auto Save.
        ui::BindHeader();

        // Per-row focus column (0 = keyboard, 1 = controller), remembered across
        // frames so the highlight stays where the user left it on each row.
        static int s_curMenu = 0, s_curUp = 0, s_curDown = 0;

        KeybindActionRow("Open Menu", "Opens and closes this menu.",
                         &s_curMenu, &st.openKeyVk, &st.openPadMask,
                         def.openKeyVk, def.openPadMask,
                         BindTarget::MenuKey, BindTarget::MenuPad);
        KeybindActionRow("Fly Up", "While Free Flight is on and you're airborne, hold this to rise.",
                         &s_curUp, &st.flyUpKeyVk, &st.flyUpPadMask,
                         def.flyUpKeyVk, def.flyUpPadMask,
                         BindTarget::FlyUpKey, BindTarget::FlyUpPad);
        KeybindActionRow("Fly Down", "While Free Flight is on and you're airborne, hold this to sink.",
                         &s_curDown, &st.flyDownKeyVk, &st.flyDownPadMask,
                         def.flyDownKeyVk, def.flyDownPadMask,
                         BindTarget::FlyDownKey, BindTarget::FlyDownPad);

        st.rebindCapture = DriveRebindCapture(st);

        if (ui::Option("Reset All Keybinds",
                       "Resets every keyboard and controller bind on this page to its default."))
        {
            Settings::ResetBinds();
            Settings::Save(); // binds persist regardless of Auto Save
            s_capTarget = BindTarget::None;
            ui::Toast("Keybinds reset to defaults");
        }

        ui::End();
    }

    static void RenderSystem()
    {
        State& st = State::Get();
        ui::Begin();

        // `save` = write Trinity.ini this frame. The Auto Save flag flip is
        // always written (so turning it OFF is remembered); everything else
        // only writes while Auto Save is on.
        bool save = false;

        ui::Submenu("Keybinds", "keybinds",
                   "Set the keyboard and controller binds for opening the menu and Free Flight.");

        // Accent theme. Only the accent changes - the greys stay put, so no
        // choice here can make the menu harder to read.
        {
            static const char* s_themeNames[8];
            const int tn = ui::ThemeCount() < 8 ? ui::ThemeCount() : 8;
            for (int i = 0; i < tn; ++i) s_themeNames[i] = ui::ThemeName(i);
            int sel = st.themeIndex;
            if (ui::Combo("Menu Theme", &sel, s_themeNames, tn,
                          "Accent colour for the menu."))
            {
                st.themeIndex = sel;
                ui::SetTheme(sel);
                save = true;
            }
        }

        // Language. Built from the files actually installed, so it only appears
        // when there is something to switch to.
        if (i18n::LanguageCount() > 1)
        {
            static const char* s_langNames[16];
            const int ln = i18n::LanguageCount() < 16 ? i18n::LanguageCount() : 16;
            for (int i = 0; i < ln; ++i) s_langNames[i] = i18n::LanguageName(i);

            int sel = i18n::CurrentLanguage();
            if (ui::Combo("Language", &sel, s_langNames, ln,
                          "Menu language. Applies immediately."))
            {
                const bool wasCjk = i18n::NeedsCjkGlyphs();
                i18n::SetLanguage(sel);
                snprintf(st.language, sizeof(st.language), "%s", i18n::LanguageCode(sel));

                // Save immediately rather than through autoSave. Every other
                // setting can wait for a save, but this one is read back before
                // the menu even exists (the font atlas is built from it), so
                // leaving it unsaved made the language unusable for anyone with
                // autoSave off - which is exactly what happened in testing.
                Settings::Save();

                // Latin and CJK need different glyph sets, so crossing that
                // line means the atlas has to be rebuilt. Anything else is just
                // different text in the glyphs already loaded.
                if (i18n::NeedsCjkGlyphs() != wasCjk)
                    ui::RequestFontRebuild();
            }
        }

        if (ui::Toggle("File Logging", &st.fileLogging,
                       "Writes Trinity.log next to the mod. Leave this on unless you have a "
                       "reason not to - without it a bug report has nothing to go on."))
        {
            if (st.fileLogging) trinity::Logger::EnableFile();
            else                trinity::Logger::DisableFile();
            save = true;
        }

        save |= ui::Toggle("Item Preview", &st.itemPreview,
                           "Shows the highlighted item large, beside the menu. The row "
                           "icons are too small to judge how a weapon or armour actually "
                           "looks.") && st.autoSave;

        save |= ui::Toggle("Show FPS Counter", &st.showFps, "Shows your FPS in the corner of the screen.") && st.autoSave;

        if (ui::Toggle("Auto Save Features", &st.autoSave,
                       "Saves your settings automatically and restores them next time."))
            save = true;
        if (ui::Option("Reset All to Default",
                       "Resets every feature to its default."))
        {
            Settings::ResetFeatures();
            save |= st.autoSave;
            ui::Toast("All features reset to defaults");
        }

        if (save)
            Settings::Save();

        ui::End();
    }

    void Render()
    {
        State&   st = State::Get();
        ImGuiIO& io = ImGui::GetIO();

        static bool s_tabsSet = (ui::SetTabs(kTabs, TabCount), true);
        (void)s_tabsSet;

        // The menu is keyboard/d-pad driven and leaves the mouse to the game so
        // the player can still look around, so we never draw an ImGui cursor.
        io.MouseDrawCursor = false;

        // Toasts outlive the menu (e.g. "Warping to..." after closing it).
        ui::DrawToasts();

        // A queued dye apply finishes on the game thread; report it wherever
        // the user is (the "Applying dye..." toast keeps this path drawing
        // even if they closed the menu right after).
        ReportPendingDye();

        if (st.showFps)
            DrawFpsCounter();

        if (!st.menuOpen)
            return;

        ui::BeginFrame();

        const char* cur = ui::CurrentMenu();

        // A rebind capture only lives on the SYSTEM tab's Keybinds submenu. If
        // we somehow left it (a mouse tab-click, or backing out while
        // listening), abandon the capture so menu navigation never stays
        // frozen elsewhere.
        if (st.rebindCapture && (ui::CurrentTab() != TabSystem || strcmp(cur, "keybinds") != 0))
            st.rebindCapture = false;

        if (!*cur)
        {
            switch (ui::CurrentTab())
            {
            case TabPlayer: RenderPlayer(); break;
            case TabTravel: RenderTravel(); break;
            case TabInventory: RenderInventoryHome(); break;
            case TabWorld:  RenderWorld();  break;
            case TabSystem: RenderSystem(); break;
            default:        RenderPlayer(); break;
            }
        }
        else if (!strcmp(cur, "keybinds")) RenderKeybinds();
        else if (!strcmp(cur, "savedloc")) RenderSavedLocations();
        else if (!strcmp(cur, "ftcats"))   RenderFastTravelCats();
        else if (!strcmp(cur, "ftnodes"))  RenderFastTravelNodes();
        else if (!strcmp(cur, "dyeslots"))  RenderDyeSlots();
        else if (!strcmp(cur, "dyeedit"))   RenderDyeEdit();
        else if (!strcmp(cur, "dyecustom")) RenderDyeCustom();
        else if (!strcmp(cur, "equipslots")) RenderEquipSlots();
        else if (!strcmp(cur, "equipedit"))  RenderEquipEdit();
        else if (!strcmp(cur, "equipgear"))  RenderEquipGear();
        else if (!strcmp(cur, "invedit"))  RenderInventoryEditor();
        else if (!strcmp(cur, "invstore")) RenderInventoryStorage();
        else if (!strcmp(cur, "invcat"))   RenderInventoryCat();
        else if (!strcmp(cur, "invlost"))   RenderInventoryLost();
        else if (!strcmp(cur, "invspecial")) RenderInventorySpecial();
        else if (!strcmp(cur, "invcamp"))   RenderInventoryCamp();
        else if (!strcmp(cur, "invadd"))    RenderInventoryAdd();
        else if (!strcmp(cur, "invaddcat")) RenderInventoryAddCat();
        else                              RenderPlayer();
    }
}
