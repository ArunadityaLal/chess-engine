#pragma once

#include <cstdint>
#include <istream>
#include <string>

#include "types.h"

// Efficiently updatable neural network evaluation, trained only on the Learned line's own
// self-play (tools/nnue/train.cpp). Architecture: 768 inputs (piece type and color x
// square, seen from each side) -> 256 hidden units per side -> 1 output. The hidden layer
// ("accumulator") is updated incrementally as pieces move, so each evaluation only has
// to run the small output layer.
namespace NNUE {

constexpr int INPUTS = 768;
constexpr int HIDDEN = 256;
constexpr int QA     = 255;  // hidden layer quantization: 1.0 = 255
constexpr int QB     = 64;   // output weights: 1.0 = 64
constexpr int SCALE  = 400;  // network output 1.0 = 400 centipawns

// Hidden layer from White's and from Black's point of view.
struct Accumulator {
    alignas(64) int16_t v[COLOR_NB][HIDDEN];
};

extern bool enabled;  // the active evaluation is the network: keep accumulators current

// Input index of a piece on a square, from perspective p: own pieces first, the board
// mirrored for Black so both sides see their pieces "from the bottom".
constexpr int feature(Color p, Piece pc, Square s) {
    return ((color_of(pc) == p ? 0 : 6) + type_of(pc)) * 64 + (p == WHITE ? s : s ^ 56);
}

// File format (little endian): "ENNUE1", int32 hidden size, int16 w1[768][H], int16 b1[H],
// int16 wo[2H] (side to move's half first), int32 bo.
bool        load(const std::string& path, std::string& error);
bool        load(std::istream& in, std::string& error);
bool        loaded();
std::string file();  // path of the loaded network

void reset(Accumulator& acc);  // biases only: the empty board
void add(Accumulator& acc, Piece pc, Square s);
void remove(Accumulator& acc, Piece pc, Square s);
void move(Accumulator& acc, Piece pc, Square from, Square to);

int evaluate(const Accumulator& acc, Color stm);  // centipawns, side to move's view

}  // namespace NNUE
