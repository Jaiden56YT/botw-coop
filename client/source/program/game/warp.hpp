#pragma once

#include <common.hpp>

/* Warp to a partner: hold the left stick click for 1.5 seconds and Link lands
   just behind your partner, if you're in the same area (both in the
   overworld, or both in the same shrine). With several partners, each use
   goes to the next one. */
namespace coop::game::warp {

    /* Game thread, every tick. */
    void Tick(s64 now_ms);
}
