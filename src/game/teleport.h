#pragma once

#include <cstddef>

namespace trinity::game
{
    // Everything related to teleporting: live position tracking, jumping to
    // an arbitrary point, and saved presets. See offsets.h (kSig_MoveUpdate)
    // for the RE background - live-verified to track the local player's own
    // coordinates; writes reuse the same tracked move-owner pointer.
    class Teleport
    {
    public:
        // Installs the movement-update hook. Requires MH_Initialize() first.
        static bool Install();
        static void Remove();

        // Last-seen live position (local x,y,z) and, when requested, the
        // coherent world origin captured with it. Returns false if no write has
        // been observed yet (e.g. still at the main menu).
        static bool GetLastPosition(float* x, float* y, float* z,
                                    float* originX = nullptr,
                                    float* originY = nullptr,
                                    float* originZ = nullptr);

        // Last-seen map destination (world x,y,z). Updated whenever the game
        // copies the destination into the marker manager. Returns false if no
        // destination has been set this session.
        static bool GetDestinationPosition(float* x, float* y, float* z);

        // Move the player to a local position. Queued as an absolute world
        // target, applied on the next movement tick, and followed by a short
        // invulnerability window so a bad landing cannot kill you. False = not
        // in the world yet.
        static bool WarpTo(float x, float y, float z);
        // Warp to the map/quest destination. Unlike a raw coordinate warp, this
        // also stamps the destination vector at moveOwner+0x1B0 so the engine's
        // own pathing/marker system accepts the teleport instead of correcting
        // the proxy position back every frame.
        static bool WarpToDestination();
        // --- Saved locations ---------------------------------------------
        // Named world coordinates kept for the current game session only.
        static size_t BookmarkCount();
        static bool   GetBookmark(size_t i, char* nameOut, size_t nameCap,
                                  float* x, float* y, float* z);
        static bool   AddBookmarkHere(const char* name);   // uses the live position
        static bool   RenameBookmark(size_t i, const char* name);
        static bool   DeleteBookmark(size_t i);
        static bool   WarpToBookmark(size_t i);
        // --- Map-marker search (research) --------------------------------
        // Three-step memory search for wherever the world map keeps the
        // marker you placed. Put a marker on a saved location, run step 1;
        // move it to a second saved location, run step 2; move it once more
        // (or to your current position) and run step 3. Results go to the log.
        //
        // If `useY` is true the scan looks for an X/Y/Z vec3 using the saved
        // location's height as well as its X/Z. That is much sharper, but only
        // works if the marker actually stores a Y. The default X/Z-only mode
        // stays available because field evidence (a reference mod ships a
        // `markerFallbackHeight` value) suggests the marker may only keep X/Z.
        // Reads only - nothing is ever written by these.
        static int    MarkerSearchStep1(float x, float y, float z, bool useY);
        static int    MarkerSearchStep2(float x, float y, float z, bool useY);
        static int    MarkerSearchStep3(float x, float y, float z, bool useY);

        // Tolerance for the marker search, in world units. Default is 5; a
        // tighter value removes false positives but may miss the marker if the
        // map snaps the pin away from the saved spot. Loosen it only if Step 1
        // returns zero with Y filter off.
        static void   SetMarkerTolerance(float units);

        // True on frames where Free Flight is actively driving the player's
        // vertical velocity (a direction is held while airborne). Exposed so the
        // HUD can light "FLY".
        static bool GetFlightEngaged();

        // Copies the last-seen position to the system clipboard as plain
        // text ("X Y Z"), ready to paste somewhere as a future preset.
        // Returns false if there's no position yet or the clipboard write
        // failed.
        static bool CopyPositionToClipboard();

        // --- Fast travel / map-gimmick catalog -----------------------------
        // The game's own fast-travel network is the LevelGimmickSceneObjectInfo
        // registry. Scenes flagged _useTeleport are the REAL waypoint networks
        // (the same filter the world map uses); they are listed first, region-
        // grouped and named by the game's own area boxes ("Hernand 0021").
        // Every other scene (ores, chests, shops, bells...) follows as a named
        // POI category - travel to those is best-effort. TravelToNode fires the
        // game's real, streaming-correct fast travel (sub_505140).
        //
        // LoadCatalog() only REQUESTS a build: the catalog is assembled on the
        // game thread (the area-name table lazy-loads its rows there), so it
        // returns false for a frame or two, then CatalogReady() flips once
        // in-world. Call it every frame while the menu is open (cheap).
        // Waypoint node lists are prebuilt; POI lists build on first access
        // (EnsureCategoryNodes, menu thread, raw reads only).
        static bool   LoadCatalog();
        static bool   CatalogReady();
        static size_t CategoryCount();
        static bool   GetCategory(size_t cat, const char** name, size_t* nodeCount);

        static bool   EnsureCategoryNodes(size_t cat);
        static size_t NodeCount(size_t cat);
        static bool   GetNode(size_t cat, size_t node, const char** label,
                              float* x, float* y, float* z);

        // Queues the game's fast travel to a catalog node. The call is dispatched
        // on the game thread (from the movement hook), matching how the game does
        // it. Returns false if the indices are out of range or no travel function
        // was resolved.
        static bool   TravelToNode(size_t cat, size_t node);
    };
}
