#pragma once

#include <common.hpp>

/* Co-op revive. When a hit would kill you, the game first tries its own
   revives (a fairy, Mipha's Grace). If it has none and a partner is with you
   (connected, same area), you're revived with 3 hearts instead of a game
   over. Once every 2 minutes; your partner's game is told you went down. */
namespace coop::game::revive {

    /* Startup: hooks the player's revive check. */
    void Install();

    /* Game thread, once per tick (tracks whether a partner is with you). */
    void Tick(s64 now_ms);

    /* Network thread: a partner went down and was revived. */
    void OnPartnerRevived(u8 player_id);

    /* Debug: in the world, set your life to 0 and run the game's revive
       check once, the way a lethal hit does. */
    void SetTest(bool on);
    /* Debug: once a partner is near, drop Link's life to this (quarter hearts)
       and skip fairies, so a real hit tests the revive. 0 = off. */
    void SetTestLife(int life);
}
