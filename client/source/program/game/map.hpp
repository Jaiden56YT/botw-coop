#pragma once

#include <common.hpp>

#include "../net/client.hpp"

/* Partner markers on the map: each other player is shown as a map stamp at
   their position, updated every second, removed when they leave.

   Stamps are game data (MapApp_MapIconNo / MapApp_MapIconPos, 200 slots), so
   the mod borrows a free slot. Its stamps carry a tag in the low bits of the
   position floats, so one left in a save (the game saved while a partner was
   on the map) is recognised and removed the next time the save is played.
   Your own stamps are never touched.

   Shared stamps: the stamps you place on your map (up to 40) are sent to your
   partners and shown on theirs, so you can mark "meet here". They carry a
   second tag, are removed when you remove yours or leave, and are never sent
   on. Ten stamp slots are always left free for the player. */
namespace coop::game::map {

    /* Call once at startup, before the network thread runs. */
    void Init();

    /* Game thread, every tick. */
    void Tick(s64 now_ms);

    /* Network thread: the stamps a partner has on their own map now. */
    void OnRemoteStamps(u8 from, const net::SharedStamp* stamps, int count);
}
