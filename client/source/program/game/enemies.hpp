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
    /* The hook forgets which sequence list is whose (a load is on: the addresses are about to be other actors'). */
    void Stop();

    /* Network thread: where a partner's enemies are. */
    void OnRemote(u8 from, u32 area, const Pose* poses, int count);

    /* A sequence an enemy's owner saw the game start on it (attack, guard,
       stagger...), for the others to play on their copy. */
    struct AnimEvent {
        u32 hash;
        float a, b; /* the game's own two float arguments, passed on as they are */
        s8 slot, bank;
        u8 flag;
        char name[40];
    };
    constexpr int MaxAnimsPerMessage = 16;
    constexpr u8 RefreshFlag = 0x80; /* in AnimEvent::flag: "it's still doing this", not a new start */
    constexpr u8 ReactFlag = 0x40;   /* something that happened to it in the sender's world (a hit): every copy plays it */
    constexpr u8 NoteFlag = 0x20;    /* never sent: what a copy that isn't ours started, kept in case it becomes ours */

    /* 0: enemy sequences aren't shared. 1: a partner's enemy also plays what
       its owner's copy plays. 2: and what its own AI asks for is dropped. */
    void SetAnimSync(int mode, bool log, bool mute);

    /* From the hook on the game's "start a sequence" (any thread). 0: start
       it as asked. 1: drop the request (a partner's enemy does what its
       owner's copy does). 2: start "Wait" in its place (nobody's enemy while
       we're down stands still). */
    int OnStartAnimation(uintptr_t as_list, float a, float b, const char* name, int target, int bank, bool flag);

    /* Network thread: sequences a partner's enemies started. */
    void OnRemoteAnims(u8 from, u32 area, const AnimEvent* events, int count);
}
