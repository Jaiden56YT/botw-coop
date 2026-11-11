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
    bool GoToPartner(u8 id, s64 now_ms);
    /* Test helper (test_follow): keep warping to the nearest partner once they're this far away. */
    void SetTestFollow(int metres);
}
