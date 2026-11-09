#pragma once

#include <common.hpp>

namespace coop::game {

    struct GhostSettings {
        char actor_name[64]; /* actor spawned to stand in for another player */
        bool echo;           /* debug: also show a ghost of yourself, ~1 s behind */
        char test_spawn[256]; /* debug: comma-separated actors spawned once in a row in front of Link */
    };

    /* Hooks the game so remote players are shown as ghost actors that follow
       their reported positions. Call once from startup. */
    void InstallGhosts();

    /* Called by the network thread once settings are loaded. */
    void ConfigureGhosts(const GhostSettings& settings);
}
