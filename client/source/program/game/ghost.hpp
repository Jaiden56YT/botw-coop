#pragma once

#include <common.hpp>

namespace coop::game {

    struct GhostSettings {
        char actor_name[64]; /* actor spawned to stand in for another player */
        bool echo;           /* debug: also show a ghost of yourself, ~1 s behind */
        char test_spawn[256]; /* debug: comma-separated actors spawned once in a row in front of Link */
        bool test_move;       /* debug: walk/run/jump the local Link and log its animations */
    };

    /* Spawns an actor by name at a position (game thread). 0 on failure. */
    uintptr_t SpawnActor(const char* name, const float (&pos)[3], float rot_y);

    /* Hooks the game so remote players are shown as ghost actors that follow
       their reported positions. Call once from startup. */
    void InstallGhosts();

    /* The local player's current anim::Category (from its own animation system). */
    u32 LocalAnimCategory();

    /* Whether actors may be created right now. Not during a loading screen:
       creating one there stalls the load for good (fast travel with a partner
       connected never finished). The game's clock stands still while it
       loads, so: only once the clock has run for 1.5 s, and not within 3 s of
       the game removing one of our actors (how a load starts). Game thread. */
    bool SafeToSpawn(s64 now_ms);

    /* Plays one of the local player's own body sequences ("Dead", "Wait"). Game thread. */
    bool PlayLocalAS(const char* name);
    /* While on, the game's own body sequences for the local player are dropped,
       so the one started with PlayLocalAS stays. */
    void HoldLocalPose(bool on);

    /* Called by the network thread once settings are loaded. */
    void ConfigureGhosts(const GhostSettings& settings);
}
