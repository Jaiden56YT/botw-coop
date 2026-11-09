#pragma once

#include <common.hpp>

namespace coop::game {

    /* Reads Link's world position from PlayerInfo. Returns false while there is
       no player (title screen, loading). Safe to call from any thread: it only
       reads three floats, and a torn read just gives one odd sample. */
    bool GetLocalPlayerPos(float (&out)[3]);

    /* Same, plus facing (yaw in radians, 0 = +Z). */
    bool GetLocalPlayer(float (&pos)[3], float* rot_y);

    /* Link has been placed at a save position: the save is loading or loaded
       (the loading screen may still be up). Looser than GetLocalPlayer. */
    bool IsPlayerPlaced();

    /* Debug: logs PlayerInfo's view of the player next to the actor's own matrix. */
    void LogPlayerDebug();
}
