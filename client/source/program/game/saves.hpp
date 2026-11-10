#pragma once

#include <common.hpp>

/* Separate co-op saves. Shared progress (shrines, towers, Koroks, rewards)
   is written into the save you play, so by default the mod keeps co-op play
   in its own copy: every save file the game opens under its save mount is
   redirected into a coop/ folder there ("save:/0/game_data.sav" becomes
   "save:/coop/0/game_data.sav"). The first time, your saves are copied
   into coop/, so co-op starts where you were; after that the two never mix.
   `separate_save = false` in config.ini turns this off. */
namespace coop::game::saves {

    /* Call at startup, before the game mounts its save (from the nnMain hook). */
    void Install();
}
