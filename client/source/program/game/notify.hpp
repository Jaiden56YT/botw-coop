#pragma once

#include <common.hpp>

/* On-screen messages: the game's info banner (where "The Master Sword has
   returned to the forest" shows), with the text swapped for ours. Two of the
   banner's messages are borrowed (MainScreen_00 "0028" and "0061"); the swap
   only applies for 2 s after we show one, so the game's own still work. */
namespace coop::game::notify {

    void Install();

    /* Any thread. Shown one after another, about 3.5 s apart. */
    void Show(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

    /* Game thread, every tick. */
    void Tick(s64 now_ms);

    /* A gift went to or came from a partner. The same gift several times in a
       row makes one message ("FakeA gave you Apple x5"). Game thread. */
    void Gift(bool to_me, const char* partner, const char* actor);

    /* The in-game name of an item or weapon actor ("Apple" for Item_Fruit_A),
       in the game's language. Game thread. False if unknown. */
    bool DisplayName(const char* actor, char* out, size_t cap);
}
