#pragma once

#include <common.hpp>

/* Debug only: runtime searches used while reverse-engineering (test_research). */
namespace coop::game::research {

    void SetTest(int mode);
    void Tick(s64 now_ms);
    /* Mode 4: who asks for the player's position, and how often (to tell play from the pause menu). */
    void NoteCaller(uintptr_t return_address);
}
