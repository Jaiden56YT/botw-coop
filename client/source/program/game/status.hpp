#pragma once

#include <common.hpp>

/* Partner travel: a banner when a partner fast-travels or warps (with how
   far away they are now, and that L3 follows them), warps to you, or goes
   into or out of a shrine.

   Partner health: a banner when a partner drops low on hearts ("FakeA is low
   on hearts: 2.5 hearts left, 230 m away"), once until they're back above
   40%, and at most every 30 s. Going down and being revived have their own
   messages (revive). */
namespace coop::game::status {

    /* Game thread, every tick. */
    void Tick(s64 now_ms);
}
