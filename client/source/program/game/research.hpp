#pragma once

#include <common.hpp>

/* Debug only: runtime searches used while reverse-engineering (test_research). */
namespace coop::game::research {

    void SetTest(int mode);
    void Tick(s64 now_ms);
}
