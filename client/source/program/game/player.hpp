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

    /* Which area Link is in: one id for the whole overworld, and one per shrine,
       Divine Beast or other separate stage. 0 = unknown (loading). */
    u32 LocalArea();

    /* Link's hearts in quarter hearts (life, max life). Zeros if unknown. */
    void LocalLife(u16* life, u16* max_life);
    u32 OverworldArea();
    /* Debug: the stage strings behind LocalArea. */
    const char* LocalMapType();
    const char* LocalMapName();

    /* The title screen's Link (the menu backdrop) is up: no save is being played. */
    bool IsAtTitle();

    /* Debug: logs PlayerInfo's view of the player next to the actor's own matrix. */
    void LogPlayerDebug();
}
