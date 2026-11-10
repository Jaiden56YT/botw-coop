#pragma once

#include <common.hpp>

/* Shared enemies: the same placed enemy (same map HashId) exists in every
   player's world. When one player hurts it, its life drops for everyone, so
   hits from both players count and a kill in one world is a kill in all. */
namespace coop::game::combat {

    /* Call once at startup, before the network thread runs. */
    void Init();

    /* Game thread, every tick. */
    void Tick(s64 now_ms);

    /* Network thread: a partner's enemy changed life. */
    void OnRemoteLife(u32 area, u32 hash_id, s32 life);

    /* Debug: log enemies near Link (name, HashId, life) once in the world. */
    void SetLogNearby(bool on);
}
