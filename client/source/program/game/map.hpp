#pragma once

#include <common.hpp>

/* Partner markers on the map: each other player is shown as a map stamp at
   their position, updated every second, removed when they leave.

   Stamps are game data (MapApp_MapIconNo / MapApp_MapIconPos, 200 slots), so
   the mod borrows a free slot. Its stamps carry a tag in the low bits of the
   position floats, so one left in a save (the game saved while a partner was
   on the map) is recognised and removed the next time the save is played.
   Your own stamps are never touched. */
namespace coop::game::map {

    /* Game thread, every tick. */
    void Tick(s64 now_ms);
}
