#pragma once

namespace coop::game {

    /* Test helper for unattended runs: when enabled, taps A every 1.5 s at the
       title and menus until Link exists in the world, then turns itself off for
       good. Off unless config.ini says `autostart = true`. */
    void InstallAutostart();
    void EnableAutostart(bool enabled);
    void LogAutostartStats();
}
