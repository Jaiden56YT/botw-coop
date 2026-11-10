#pragma once

#include <common.hpp>

/* What the player's body is doing, as a small number that fits in
   PlayerState::anim_hash. 0 = unknown (the puppet guesses from speed). */
namespace coop::game::anim {

    enum Category : u32 {
        Unknown = 0,
        Wait, Move, Climb, ClimbWait, Swim, SwimWait, SwimDash, Glide, Jump, Fall, Horse, Surf,
        Count,
    };

    /* Maps one of the player's own animation-sequence names (GameROMPlayer's
       ASList: "Wait", "Move", "ClimbMove", "ParashawlGlide", ...) to a category. */
    Category FromPlayerAS(const char* name);

    /* The puppet's sequence for a category; Move picks walk/run/sprint from speed. */
    const char* PuppetAS(Category c, float speed);

    /* What the player is doing with their hands, on top of the movement
       category: sent in bits 8-15 of PlayerState::anim_hash. */
    enum Action : u32 {
        NoAction = 0,
        Attack1, Attack2, Attack3, AttackF, Spin, ChargeWait, BowAim, BowShoot, ThrowAim, Throw, Guard,
        ActionCount,
    };

    /* An action started by one of the player's sequences in any slot (0 body,
       1 upper body, 3 face), and how long to show it; NoAction if none.
       Sets *ends_hold for sequences that end the long actions (aiming, charging). */
    Action ActionFromPlayerAS(const char* name, int slot, int* duration_ms, bool* ends_hold);

    /* The puppet's sequence for an action. */
    const char* PuppetActionAS(Action a);
}
