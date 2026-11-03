#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "position.h"

// Training positions from self-play, 32 bytes each, for the Learned eval's tuner and the
// NNUE trainer. Only what an evaluation needs is kept: pieces, side to move, the
// search's score and the game's result (no castling, en passant or move counters).
struct PackedPos {
    uint64_t occupancy;   // squares with a piece
    uint8_t  pieces[16];  // 4 bits per piece (Piece enum), in occupancy order, low nibble first
    int16_t  score;       // search score in centipawns, from White's side
    uint8_t  result;      // 0 Black won, 1 draw, 2 White won
    uint8_t  stm;         // side to move
    uint8_t  reserved[4] = {};

    static PackedPos pack(const Position& pos, int whiteScore, int result);
    std::string      fen() const;
    float            white_result() const { return result / 2.0f; }
};
static_assert(sizeof(PackedPos) == 32);

namespace Datagen {

struct Options {
    int         games       = 100;
    int         nodes       = 5000;  // per move
    int         randomPlies = 8;     // random opening moves (8 or 9, so both sides start)
    uint64_t    seed        = 1;
    std::string out;                 // .bin file; a .meta text file is written next to it
};

// Plays self-play games with the current evaluation (set Eval/EvalFile first) and appends
// quiet positions to the output. Prints progress as info strings.
void run(const Options& options);

// Reads a .bin file (all of it, or the first `limit` positions).
std::vector<PackedPos> load(const std::string& path, size_t limit = 0);

// The "eval" line of a data file's .meta, e.g. "Learned gen3.txt", or empty if unknown.
std::string source_eval(const std::string& binPath);

}  // namespace Datagen
