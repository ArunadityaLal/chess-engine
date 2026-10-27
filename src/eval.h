#pragma once

#include <istream>
#include <string>

#include "types.h"

class Position;

// A middlegame and an endgame value, blended by game phase ("tapered" evaluation).
struct Score {
    int mg = 0, eg = 0;

    constexpr Score& operator+=(Score o) { mg += o.mg; eg += o.eg; return *this; }
    constexpr Score& operator-=(Score o) { mg -= o.mg; eg -= o.eg; return *this; }
    constexpr bool   operator==(const Score&) const = default;
};

constexpr Score operator+(Score a, Score b) { return {a.mg + b.mg, a.eg + b.eg}; }
constexpr Score operator-(Score s) { return {-s.mg, -s.eg}; }

// Phase 24 = all knights, bishops, rooks and queens on the board (pure middlegame);
// 0 = only kings and pawns (pure endgame).
constexpr int PHASE_MAX = 24;

namespace Eval {

// Two evaluations with the same shape (piece values + piece-square tables):
//  PeSTO:   tuned by others on game positions. The fixed reference opponent.
//  Learned: starts with piece values only and is tuned on the engine's own games, never
//           on PeSTO's. Weights come from EvalFile; without one it is material only.
enum class Kind { PeSTO, Learned };

// PieceSquare[pc][sq] = material + placement of that piece on that square, from white's
// point of view (negative for black pieces), for the active evaluation. Position keeps a
// running sum of these.
extern Score PieceSquare[PIECE_NB][SQUARE_NB];
extern int   PhaseWeight[PIECE_TYPE_NB];

void init();  // call before constructing any Position

// Switch evaluations. Positions built before the switch hold stale sums: call
// Position::refresh_eval() on any that are kept.
void use(Kind kind);
Kind active();

// Learned weights in the text format params() prints. On error, returns false with a
// message and keeps the previous weights. An empty path means material only.
bool load_learned(const std::string& path, std::string& error);
bool load_learned(std::istream& in, std::string& error);

int         evaluate(const Position& pos);  // centipawns, from the side to move's view
std::string trace(const Position& pos);     // human-readable breakdown of evaluate()
std::string params();                       // the active weights, as an EvalFile
std::string format_score(int cp);           // 123 -> "+1.23"

}  // namespace Eval
