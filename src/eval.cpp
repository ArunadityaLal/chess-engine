#include "eval.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <vector>

#include "position.h"

namespace Eval {
Score PieceSquare[PIECE_NB][SQUARE_NB];
int   PhaseWeight[PIECE_TYPE_NB] = {0, 1, 1, 2, 4, 0};
}  // namespace Eval

namespace {

// PeSTO by Ronald Friederich: piece values and piece-square tables fitted to game
// results by Texel tuning. General principles only, no opening or endgame lines.
// https://www.chessprogramming.org/PeSTO%27s_Evaluation_Function
constexpr Score PestoValue[PIECE_TYPE_NB] = {
    {82, 94}, {337, 281}, {365, 297}, {477, 512}, {1025, 936}, {0, 0}};

// Placement bonuses for white, laid out like a board diagram: the first row is rank 8.
constexpr int MgTable[PIECE_TYPE_NB][64] = {
    {   0,   0,   0,   0,   0,   0,   0,   0,  // pawn
       98, 134,  61,  95,  68, 126,  34, -11,
       -6,   7,  26,  31,  65,  56,  25, -20,
      -14,  13,   6,  21,  23,  12,  17, -23,
      -27,  -2,  -5,  12,  17,   6,  10, -25,
      -26,  -4,  -4, -10,   3,   3,  33, -12,
      -35,  -1, -20, -23, -15,  24,  38, -22,
        0,   0,   0,   0,   0,   0,   0,   0},
    {-167, -89, -34, -49,  61, -97, -15,-107,  // knight
      -73, -41,  72,  36,  23,  62,   7, -17,
      -47,  60,  37,  65,  84, 129,  73,  44,
       -9,  17,  19,  53,  37,  69,  18,  22,
      -13,   4,  16,  13,  28,  19,  21,  -8,
      -23,  -9,  12,  10,  19,  17,  25, -16,
      -29, -53, -12,  -3,  -1,  18, -14, -19,
     -105, -21, -58, -33, -17, -28, -19, -23},
    { -29,   4, -82, -37, -25, -42,   7,  -8,  // bishop
      -26,  16, -18, -13,  30,  59,  18, -47,
      -16,  37,  43,  40,  35,  50,  37,  -2,
       -4,   5,  19,  50,  37,  37,   7,  -2,
       -6,  13,  13,  26,  34,  12,  10,   4,
        0,  15,  15,  15,  14,  27,  18,  10,
        4,  15,  16,   0,   7,  21,  33,   1,
      -33,  -3, -14, -21, -13, -12, -39, -21},
    {  32,  42,  32,  51,  63,   9,  31,  43,  // rook
       27,  32,  58,  62,  80,  67,  26,  44,
       -5,  19,  26,  36,  17,  45,  61,  16,
      -24, -11,   7,  26,  24,  35,  -8, -20,
      -36, -26, -12,  -1,   9,  -7,   6, -23,
      -45, -25, -16, -17,   3,   0,  -5, -33,
      -44, -16, -20,  -9,  -1,  11,  -6, -71,
      -19, -13,   1,  17,  16,   7, -37, -26},
    { -28,   0,  29,  12,  59,  44,  43,  45,  // queen
      -24, -39,  -5,   1, -16,  57,  28,  54,
      -13, -17,   7,   8,  29,  56,  47,  57,
      -27, -27, -16, -16,  -1,  17,  -2,   1,
       -9, -26,  -9, -10,  -2,  -4,   3,  -3,
      -14,   2, -11,  -2,  -5,   2,  14,   5,
      -35,  -8,  11,   2,   8,  15,  -3,   1,
       -1, -18,  -9,  10, -15, -25, -31, -50},
    { -65,  23,  16, -15, -56, -34,   2,  13,  // king
       29,  -1, -20,  -7,  -8,  -4, -38, -29,
       -9,  24,   2, -16, -20,   6,  22, -22,
      -17, -20, -12, -27, -30, -25, -14, -36,
      -49,  -1, -27, -39, -46, -44, -33, -51,
      -14, -14, -22, -46, -44, -30, -15, -27,
        1,   7,  -8, -64, -43, -16,   9,   8,
      -15,  36,  12, -54,   8, -28,  24,  14},
};

constexpr int EgTable[PIECE_TYPE_NB][64] = {
    {   0,   0,   0,   0,   0,   0,   0,   0,  // pawn
      178, 173, 158, 134, 147, 132, 165, 187,
       94, 100,  85,  67,  56,  53,  82,  84,
       32,  24,  13,   5,  -2,   4,  17,  17,
       13,   9,  -3,  -7,  -7,  -8,   3,  -1,
        4,   7,  -6,   1,   0,  -5,  -1,  -8,
       13,   8,   8,  10,  13,   0,   2,  -7,
        0,   0,   0,   0,   0,   0,   0,   0},
    { -58, -38, -13, -28, -31, -27, -63, -99,  // knight
      -25,  -8, -25,  -2,  -9, -25, -24, -52,
      -24, -20,  10,   9,  -1,  -9, -19, -41,
      -17,   3,  22,  22,  22,  11,   8, -18,
      -18,  -6,  16,  25,  16,  17,   4, -18,
      -23,  -3,  -1,  15,  10,  -3, -20, -22,
      -42, -20, -10,  -5,  -2, -20, -23, -44,
      -29, -51, -23, -15, -22, -18, -50, -64},
    { -14, -21, -11,  -8,  -7,  -9, -17, -24,  // bishop
       -8,  -4,   7, -12,  -3, -13,  -4, -14,
        2,  -8,   0,  -1,  -2,   6,   0,   4,
       -3,   9,  12,   9,  14,  10,   3,   2,
       -6,   3,  13,  19,   7,  10,  -3,  -9,
      -12,  -3,   8,  10,  13,   3,  -7, -15,
      -14, -18,  -7,  -1,   4,  -9, -15, -27,
      -23,  -9, -23,  -5,  -9, -16,  -5, -17},
    {  13,  10,  18,  15,  12,  12,   8,   5,  // rook
       11,  13,  13,  11,  -3,   3,   8,   3,
        7,   7,   7,   5,   4,  -3,  -5,  -3,
        4,   3,  13,   1,   2,   1,  -1,   2,
        3,   5,   8,   4,  -5,  -6,  -8, -11,
       -4,   0,  -5,  -1,  -7, -12,  -8, -16,
       -6,  -6,   0,   2,  -9,  -9, -11,  -3,
       -9,   2,   3,  -1,  -5, -13,   4, -20},
    {  -9,  22,  22,  27,  27,  19,  10,  20,  // queen
      -17,  20,  32,  41,  58,  25,  30,   0,
      -20,   6,   9,  49,  47,  35,  19,   9,
        3,  22,  24,  45,  57,  40,  57,  36,
      -18,  28,  19,  47,  31,  34,  39,  23,
      -16, -27,  15,   6,   9,  17,  10,   5,
      -22, -23, -30, -16, -16, -23, -36, -32,
      -33, -28, -22, -43,  -5, -32, -20, -41},
    { -74, -35, -18, -18, -11,  15,   4, -17,  // king
      -12,  17,  14,  17,  17,  38,  23,  11,
       10,  17,  23,  15,  20,  45,  44,  13,
       -8,  22,  24,  27,  26,  33,  26,   3,
      -18,  -4,  21,  24,  27,  23,   9, -11,
      -19,  -3,  11,  21,  23,  16,   7,  -9,
      -27, -11,   4,  13,  14,   4,  -5, -17,
      -53, -34, -21, -11, -28, -14, -24, -43},
};

constexpr const char* PieceNames[PIECE_TYPE_NB] = {"pawn", "knight", "bishop", "rook", "queen", "king"};

// The weights of one evaluation: piece values plus placement tables laid out like
// MgTable (white's side, rank 8 first).
struct Params {
    Score value[PIECE_TYPE_NB];
    int   mg[PIECE_TYPE_NB][64];
    int   eg[PIECE_TYPE_NB][64];
};

Params        Pesto, Learned;
const Params* Active     = &Pesto;
Eval::Kind    ActiveKind = Eval::Kind::PeSTO;

// Where the learned line starts: textbook piece values, no idea where pieces belong.
Params material_only() {
    Params    p{};
    const int values[PIECE_TYPE_NB] = {100, 300, 300, 500, 900, 0};
    for (int pt = PAWN; pt <= KING; ++pt)
        p.value[pt] = {values[pt], values[pt]};
    return p;
}

// Tables are written rank 8 first, so a white piece on square s reads row-mirrored
// entry s ^ 56; a black piece reads entry s directly (the board seen from its side).
Score placement(Piece pc, Square s) {
    const int i = color_of(pc) == WHITE ? s ^ 56 : s;
    return {Active->mg[type_of(pc)][i], Active->eg[type_of(pc)][i]};
}

void rebuild_piece_square() {
    for (int pc = W_PAWN; pc <= B_KING; ++pc)
        for (Square s = A1; s <= H8; ++s) {
            const Score v = Active->value[type_of(Piece(pc))] + placement(Piece(pc), s);
            Eval::PieceSquare[pc][s] = color_of(Piece(pc)) == WHITE ? v : -v;
        }
}

int taper(Score s, int phase) {
    phase = std::min(phase, PHASE_MAX);  // promotions can push it past the maximum
    return (s.mg * phase + s.eg * (PHASE_MAX - phase)) / PHASE_MAX;
}

std::string pad(const std::string& s, size_t width) {
    return s.size() >= width ? s : std::string(width - s.size(), ' ') + s;
}

}  // namespace

void Eval::init() {
    std::copy(std::begin(PestoValue), std::end(PestoValue), Pesto.value);
    std::copy(&MgTable[0][0], &MgTable[0][0] + PIECE_TYPE_NB * 64, &Pesto.mg[0][0]);
    std::copy(&EgTable[0][0], &EgTable[0][0] + PIECE_TYPE_NB * 64, &Pesto.eg[0][0]);
    Learned = material_only();
    use(Kind::PeSTO);
}

void Eval::use(Kind kind) {
    ActiveKind = kind;
    Active     = kind == Kind::PeSTO ? &Pesto : &Learned;
    rebuild_piece_square();
}

Eval::Kind Eval::active() { return ActiveKind; }

bool Eval::load_learned(const std::string& path, std::string& error) {
    if (path.empty()) {
        Learned = material_only();
    } else {
        std::ifstream in(path);
        if (!in) {
            error = "can't open " + path;
            return false;
        }
        if (!load_learned(in, error))
            return false;
    }
    if (ActiveKind == Kind::Learned)
        rebuild_piece_square();
    return true;
}

// Format: "value <piece> <mg> <eg>", "mg <piece> <64 numbers>", "eg <piece> <64 numbers>",
// in any order and layout; '#' starts a comment. Anything not given stays material only.
bool Eval::load_learned(std::istream& in, std::string& error) {
    std::string text, line;
    while (std::getline(in, line))
        text += line.substr(0, line.find('#')) + '\n';

    std::istringstream is(text);
    Params             p = material_only();
    std::string        key, piece;
    while (is >> key) {
        if (!(is >> piece)) {
            error = "missing piece name after '" + key + "'";
            return false;
        }
        const auto it = std::find(std::begin(PieceNames), std::end(PieceNames), piece);
        if (it == std::end(PieceNames)) {
            error = "unknown piece '" + piece + "'";
            return false;
        }
        const int pt = int(it - std::begin(PieceNames));

        if (key == "value") {
            if (!(is >> p.value[pt].mg >> p.value[pt].eg)) {
                error = "bad value for " + piece;
                return false;
            }
        } else if (key == "mg" || key == "eg") {
            int* table = key == "mg" ? p.mg[pt] : p.eg[pt];
            for (int i = 0; i < 64; ++i)
                if (!(is >> table[i])) {
                    error = "expected 64 numbers for " + key + " " + piece;
                    return false;
                }
        } else {
            error = "unknown keyword '" + key + "'";
            return false;
        }
    }
    Learned = p;
    return true;
}

std::string Eval::params() {
    std::ostringstream os;
    os << "# " << (ActiveKind == Kind::PeSTO ? "PeSTO" : "Learned")
       << " evaluation weights (centipawns). Placement tables are from White's side,\n"
       << "# drawn like a board: the first row is rank 8.\n";
    for (int pt = PAWN; pt <= KING; ++pt) {
        os << "\nvalue " << PieceNames[pt] << ' ' << Active->value[pt].mg << ' '
           << Active->value[pt].eg << '\n';
        for (const char* key : {"mg", "eg"}) {
            const int* table = std::string(key) == "mg" ? Active->mg[pt] : Active->eg[pt];
            os << key << ' ' << PieceNames[pt] << '\n';
            for (int i = 0; i < 64; ++i) {
                char buf[8];
                std::snprintf(buf, sizeof buf, "%5d", table[i]);
                os << buf << (i % 8 == 7 ? "\n" : "");
            }
        }
    }
    return os.str();
}

int Eval::evaluate(const Position& pos) {
    const int v = taper(pos.psq_score(), pos.phase());
    return pos.side_to_move() == WHITE ? v : -v;
}

std::string Eval::format_score(int cp) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "%+.2f", cp / 100.0);
    return buf;
}

std::string Eval::trace(const Position& pos) {
    struct PieceInfo {
        std::string name;
        int         value;  // tapered placement bonus
    };

    const int phase = std::min(pos.phase(), PHASE_MAX);
    Score     material[COLOR_NB], place[COLOR_NB];
    std::vector<PieceInfo> pieces[COLOR_NB];

    for (Square s = A1; s <= H8; ++s) {
        const Piece pc = pos.piece_on(s);
        if (pc == NO_PIECE)
            continue;
        const Color c = color_of(pc);
        const Score p = placement(pc, s);
        material[c] += Active->value[type_of(pc)];
        place[c] += p;
        pieces[c].push_back({std::string(1, "PNBRQK"[type_of(pc)]) + to_string(s), taper(p, phase)});
    }

    std::ostringstream os;
    os << "Evaluation function: " << (ActiveKind == Kind::PeSTO ? "PeSTO" : "Learned") << "\n\n";
    auto row = [&](const std::string& label, Score w, Score b) {
        const Score d = w + -b;
        os << pad(label, 10) << pad(std::to_string(w.mg), 7) << pad(std::to_string(w.eg), 7)
           << pad(std::to_string(b.mg), 9) << pad(std::to_string(b.eg), 7)
           << pad(std::to_string(d.mg), 9) << pad(std::to_string(d.eg), 7)
           << pad(format_score(taper(d, phase)), 10) << '\n';
    };

    os << "                 White            Black       White - Black\n"
       << "               MG     EG       MG     EG       MG     EG   blended\n";
    row("Material", material[WHITE], material[BLACK]);
    row("Placement", place[WHITE], place[BLACK]);
    row("Total", material[WHITE] + place[WHITE], material[BLACK] + place[BLACK]);

    const int white = taper(pos.psq_score(), pos.phase());
    os << "\nGame phase: " << phase << "/" << PHASE_MAX << " ("
       << phase * 100 / PHASE_MAX << "% middlegame, " << 100 - phase * 100 / PHASE_MAX
       << "% endgame weights)\n"
       << "Evaluation: " << format_score(white) << " (+ favors White)\n\n"
       << "Piece placement, blended (+ = well placed for its side):\n";

    for (Color c : {WHITE, BLACK}) {
        std::sort(pieces[c].begin(), pieces[c].end(),
                  [](const PieceInfo& a, const PieceInfo& b) { return a.value > b.value; });
        os << (c == WHITE ? "  White:" : "  Black:");
        for (const PieceInfo& p : pieces[c])
            os << ' ' << p.name << ' ' << (p.value >= 0 ? "+" : "") << p.value;
        os << '\n';
    }
    return os.str();
}
