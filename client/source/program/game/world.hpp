#pragma once

#include <common.hpp>

namespace coop::game {

    /* Shared world state owned by the host: time of day and day count. The
       host's game sends it every second; guests' games follow it. */
    struct WorldState {
        float time;   /* WM_Time: degrees, 360 = 24 h */
        s32 days;     /* WM_NumberOfDays */
    };

    /* Called every game tick (game thread). */
    void WorldTick(s64 now_ms);

    /* Network thread: the host's latest world state arrived. */
    void OnHostWorldState(const WorldState& state);
}
