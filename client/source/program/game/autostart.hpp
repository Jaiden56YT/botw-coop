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

    /* Debug: presses a comma-separated button list once Link is in the world,
       one every 1.2 s ("A", "B", "X", "Y", "L", "R", "ZL", "ZR", "Plus", "Minus",
       "Left", "Up", "Right", "Down"; "R:900" holds R for 900 ms). */
    void SetKeyScript(const char* script);
    void KeyScriptTick(s64 now_ms);

    /* Test helper: holds the left stick at (x, y), each -32767..32767, for `ms`. */
    void HoldStick(s32 x, s32 y, int ms);
}
