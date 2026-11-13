#pragma once

#include <common.hpp>

/* Warp to a partner: hold the left stick click for 1.5 seconds and Link lands
   just behind your partner, if you're in the same area (both in the
   overworld, or both in the same shrine). With several partners, each use
   goes to the next one. */
namespace coop::game::warp {

    /* Game thread, every tick. */
    void Tick(s64 now_ms);

    /* Puts Link at `pos` and stops him (Havok body, matrix, fall height). Game thread. */
    bool PlaceLink(const float (&pos)[3]);

    /* Keeps Link at pos sideways and lets him fall (a downed player in the air or in water). */
    void HoldLinkXZ(const float (&pos)[3]);

    /* Warps to this partner without a message of its own (the revive's respawn). */
    /* `travel`: a far partner, or one in another area, may be reached through a loading screen (see warp_travel). */
    bool GoToPartner(u8 id, s64 now_ms, bool travel = false);
    /* Test helper (test_follow): keep warping to the nearest partner once they're this far away. */
    /* Warping right now, or arrived within the last few seconds. */
    bool Busy(s64 now_ms);
    /* The map around Link may still be loading because of a warp: nothing may be spawned. */
    bool Loading(s64 now_ms);
    void Install();
    /* config.ini warp_direct: a far warp goes in one jump (see warp.cpp). */
    void SetDirect(bool on);
    void SetTravel(bool on);
    bool Traveling(); /* a stage change of ours has been asked for and hasn't finished loading */ /* reach far partners and partners in shrines by the game's own stage change */
    void SetTestFollow(int metres, const char* leader);
    /* A warp is under way or has just landed: hearts lost now come back.
       Gives the hearts he set off with. */
    bool Guarding(s64 now_ms, s32* start_life);
}
