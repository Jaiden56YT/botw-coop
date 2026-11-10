#pragma once

#include <common.hpp>

/* Co-op revive. When a hit would kill you, the game first tries its own
   revives (a fairy, Mipha's Grace). If it has none and a partner is in the
   same area, you go down instead of a game over: you stay where you fell on a
   quarter heart, and further hits can't finish you, for 30 s. Partners are
   told where you are; one standing next to you for 1.5 s gets you back up
   with 3 hearts. If nobody comes, you're released on that quarter heart and
   the next hit ends it. At most once every 2 minutes. */
namespace coop::game::revive {

    /* Startup: hooks the player's revive check. */
    void Install();

    /* You're down, waiting for a partner. Any thread. */
    bool IsDown();

    /* Game thread, once per tick (tracks whether a partner is with you). */
    void Tick(s64 now_ms);

    /* Network thread: a partner went down and was revived (older versions). */
    void OnPartnerRevived(u8 player_id);

    /* Network thread: a partner is down (1), back up (0) or ran out of time (2). */
    void OnPartnerDowned(u8 player_id, u8 state, const float* pos, u8 seconds_left);

    /* Debug: in the world, set your life to 0 and run the game's revive
       check once, the way a lethal hit does. */
    void SetTest(bool on);
    /* Debug: once a partner is near, drop Link's life to this (quarter hearts)
       and skip fairies, so a real hit tests the revive. 0 = off. */
    void SetTestLife(int life);
}
