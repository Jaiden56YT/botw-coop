#pragma once

#include <common.hpp>

/* Enemies moving the same way in every world. Placed enemies have the same
   map HashId everywhere (see combat). For each one, the player closest to it
   (in their own world) owns it and sends where it is and which way it faces,
   10 times a second; the others steer their copy there. Its own AI keeps
   running everywhere (animations, attacks), only its place is shared. If two
   players both think they're closest, the lower player id wins.

   An enemy is moved through its Havok rigid body (found by scanning for the
   rigid-body vtable at the actor's position, then cached), as Link is for
   warps. Setting the body's velocity does nothing (its AI drives that); its
   position sticks, so the copy is slid toward the owner's spot each tick. */
namespace coop::game::enemies {

    struct Pose {
        u32 hash;
        float pos[3];
        float rot_y;
    };
    constexpr int MaxPerMessage = 24;

    /* Call once at startup, before the network thread runs. */
    void Init();

    /* Game thread, every tick. */
    void Tick(s64 now_ms);

    /* Network thread: where a partner's enemies are. */
    void OnRemote(u8 from, u32 area, const Pose* poses, int count);
}
