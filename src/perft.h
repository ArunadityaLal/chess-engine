#pragma once

#include <cstdint>

#include "position.h"

// Number of leaf nodes of the legal move tree to the given depth.
uint64_t perft(Position& pos, int depth);

// perft, printing the subtree count under each root move. When a count is wrong,
// compare against a known-good engine's divide output to find the bad move.
uint64_t perft_divide(Position& pos, int depth);
