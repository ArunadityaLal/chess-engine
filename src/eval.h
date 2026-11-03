#pragma once

#include <cstdint>
#include <istream>
#include <string>
#include <vector>

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

// Two evaluations:
//  PeSTO:   piece values + piece-square tables tuned by others on game positions. The
//           fixed reference opponent.
//  Learned: the same tables plus classical terms (mobility, pawn structure, king safety,
//           threats, ...). Starts with piece values only and is tuned on the engine's own
//           games, never on PeSTO's. Weights come from EvalFile; without one it is
//           material only.
//  NNUE:    a neural network (nnue.h) trained only on the Learned line's games: the
//           Learned line's next stage. Needs a network file.
enum class Kind { PeSTO, Learned, NNUE };

// PieceSquare[pc][sq] = material + placement of that piece on that square, from white's
// point of view (negative for black pieces), for the active evaluation. Position keeps a
// running sum of these.
extern Score PieceSquare[PIECE_NB][SQUARE_NB];
extern int   PhaseWeight[PIECE_TYPE_NB];

void init();  // call before constructing any Position

// Switch evaluations. Positions built before the switch hold stale sums: call
// Position::refresh_eval() on any that are kept. NNUE requires NNUE::loaded().
void use(Kind kind);
Kind active();

// Learned weights in the text format params() prints. On error, returns false with a
// message and keeps the previous weights. An empty path means material only.
bool load_learned(const std::string& path, std::string& error);
bool load_learned(std::istream& in, std::string& error);
std::string learned_file();  // path of the loaded EvalFile; empty for material only

int         evaluate(const Position& pos);  // centipawns, from the side to move's view
std::string trace(const Position& pos);     // human-readable breakdown of evaluate()
std::string params();                       // the active weights, as an EvalFile
std::string format_score(int cp);           // 123 -> "+1.23"

// For Texel tuning of the Learned eval, which is linear in its weights: with the
// features of a position (coefficients, white minus black, of every weight that
// applies), evaluate() from White's side = taper(sum of coefficient * weight).
struct Feature {
    int16_t index;
    int16_t coeff;
};
int                param_count();
void               features(const Position& pos, std::vector<Feature>& out);
std::vector<Score> learned_params();  // current Learned weights, in feature order
std::string        learned_params_text(const std::vector<Score>& weights);  // as an EvalFile
std::string        param_name(int index);
// The same evaluation, written the way it reads best: each piece type's average placement
// bonus and average mobility bonus are folded into its value, since every piece has
// exactly one of each (only weights with seen[i] > 0, i.e. used in the data, count).
void normalize(std::vector<Score>& weights, const std::vector<uint64_t>& seen);

}  // namespace Eval
