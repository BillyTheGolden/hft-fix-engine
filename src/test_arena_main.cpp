/**
 * @file test_arena_main.cpp
 * @brief Entry point executable for running the HFT Resilience Test Arena.
 */

#include "hft/arena/TestArena.hpp"

int main()
{
    hft::arena::TestArena arena;
    arena.run_all_scenarios();
    return 0;
}
