#pragma once

#include <common.hpp>

#include "../net/client.hpp"

/* Weapons on the puppet. The game keeps a weapon on Link's hand or back with a
   ModelBindInfo (Actor+0x4d0): a parent actor, a bone name and an offset.
   The mod makes the same kind of bind between a CoopWeapon_<model> actor and
   the puppet. */
namespace coop::game::weapons {

    /* A bind slot: owns the ModelBindInfo memory and the bone name it points at. */
    struct Bind {
        alignas(16) u8 info[0x100];
        char bone[32];
        bool made;
    };

    /* Binds `actor` to `bone` of `parent` (identity offset when offset is null;
       else a 3x4 row-major matrix). Re-binding the same slot updates it. */
    bool BindTo(Bind& slot, uintptr_t actor, uintptr_t parent, const char* bone, const float* offset);

    /* Every frame: moves the actor onto its bone (what a weapon's own AI does). */
    void Update(Bind& slot, uintptr_t actor);

    /* Startup, before the network thread runs. */
    void Init();

    /* Network thread: a partner gave us a weapon or item (they dropped or threw it near us). */
    void OnGift(const char* from, const char* weapon);

    /* Something of ours (a weapon dropped from the inventory) is lying near us:
       give it to a partner if it ends up next to one. Game thread. */
    void TrackDropped(uintptr_t actor, const char* name, s64 now);

    /* Whether we spawned this actor for a gift we got (it isn't ours to give). */
    bool IsGiftActor(uintptr_t actor);

    /* What we carry now (as last sent). Game thread. */
    int LocalPoses(net::WeaponPose (&out)[net::MaxWeapons]);

    /* The CoopWeapon actor for a weapon actor name, or false if unknown. */
    bool WeaponActorFor(const char* weapon, char (&out)[64]);

    /* Debug: "Weapon:Bone", spawn that weapon once and bind it to Link's bone. */
    void SetTest(const char* spec);
    void Tick(s64 now_ms);
}
