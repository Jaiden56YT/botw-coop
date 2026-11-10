#pragma once

#include <common.hpp>

namespace coop::game {

    /* Test helper for unattended runs: when enabled, taps A every 1.5 s at the
       title and menus until Link exists in the world, then turns itself off for
       good. Off unless config.ini says `autostart = true`. */
    void InstallAutostart();
    void EnableAutostart(bool enabled);
    void LogAutostartStats();

    /* Test helper: holds these Npad buttons (bit 0 = A, 10 = Plus, 11 = Minus)
       for `ms` milliseconds. */
    void PressButtons(u64 mask, int ms);

    /* Test helper: holds the left stick at (x, y), each -32767..32767, for `ms`. */
    void HoldStick(s32 x, s32 y, int ms);
}
