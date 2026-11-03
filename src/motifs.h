#pragma once

#include <string>
#include <vector>

#include "position.h"

// Tactical motifs of a move, for teaching: "fork", "pin", "skewer", "discovered attack",
// "discovered check", "check", "checkmate", "hanging piece", "promotion", "sacrifice".
// Static pattern checks on the position after the move, not a search: they say what the
// move does, and the search says whether it works.
namespace Motifs {

struct Motif {
    std::string name;    // e.g. "fork"
    std::string detail;  // e.g. "Nf7 attacks Qd8 and Rh8"
};

std::vector<Motif> find(const Position& pos, Move m);

}  // namespace Motifs
