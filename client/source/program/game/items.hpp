#pragma once

#include <common.hpp>

/* Trading food and materials. BotW's menu can't drop materials, only "Hold"
   them: pick Hold, close the menu, and Link holds the items up. Put them down
   (A) or throw them next to a partner: once they're out of his hands within
   3 m of a partner, they leave your world and land at the partner's feet
   (the same Gift message weapons use).

   "Ours" = an item actor that wasn't there before and shows up right by Link
   just after the pause menu closed (the menu stops the clock, WM_Time).
   Items that fall out of grass, trees or enemies don't count. Weapons dropped
   from the menu are handed to the weapons module. */
namespace coop::game::items {

    /* Game thread, every tick. */
    void Tick(s64 now_ms);

    /* Debug: spawn this item at Link's feet once, as if dropped from the menu. */
    void SetTest(const char* name);
}
