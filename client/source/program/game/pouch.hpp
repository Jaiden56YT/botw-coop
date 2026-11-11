#pragma once

#include <common.hpp>

/* Link's inventory (the "pouch"), for trading weapons with their bonus and
   wear. A weapon on the ground is a plain actor and nothing on it says what
   bonus it had; the inventory item does. So the weapons we carry are noted
   as they are, and when one leaves the inventory (dropped from the menu or
   thrown) its bonus and durability are kept for a while, to go with the gift
   if it lands next to a partner. The partner's game puts a bonus weapon
   straight into their inventory with the game's own "got an item" call. */
namespace coop::game::pouch {

    /* A weapon's bonus (the game's WeaponModifier bits and value) and
       durability (the inventory's number: 100 per point). */
    struct WeaponInfo {
        u32 flags;      /* 0 = no bonus */
        s32 value;
        s32 durability; /* <= 0 = not known */
    };

    /* Game thread, every tick. */
    void Tick(s64 now_ms);

    /* The newest weapon of this name that left the inventory in the last two
       minutes, once. False if there's none. Game thread. */
    bool TakeRemoved(const char* name, s64 now_ms, WeaponInfo* out);

    /* Adds the weapon to the inventory with this bonus and durability. False
       if it didn't go in (no room). Game thread, not during a load. */
    bool Give(const char* name, const WeaponInfo& info);

    /* "Attack Up +44", "Durability Up", ... Empty if no bonus. */
    void Describe(const WeaponInfo& info, char* out, size_t cap);
}
