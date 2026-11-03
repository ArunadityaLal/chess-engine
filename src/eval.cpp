#include "eval.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <vector>

#include "nnue.h"
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

// Classical terms beyond material and placement, used by the Learned evaluation (PeSTO
// stays exactly PeSTO, the fixed reference). Every term is a count, or a table indexed
// by a count, so the evaluation stays linear in its weights and Texel tuning can fit
// them exactly. All start at zero: generation 0 still knows only piece values.
enum Group : int {
    MOBILITY_KNIGHT, MOBILITY_BISHOP, MOBILITY_ROOK, MOBILITY_QUEEN,
    PASSED_PAWN, PASSED_BLOCKED, PASSED_OWN_KING, PASSED_THEIR_KING,
    DOUBLED_PAWN, ISOLATED_PAWN, BACKWARD_PAWN, CONNECTED_PAWN,
    BISHOP_PAIR, ROOK_OPEN_FILE, ROOK_SEMIOPEN_FILE, KNIGHT_OUTPOST, BISHOP_OUTPOST,
    KING_ATTACK, PAWN_SHIELD, KING_OPEN_FILES,
    THREAT_BY_PAWN, THREAT_BY_MINOR, THREAT_BY_ROOK, HANGING,
    TEMPO, GROUP_NB
};

struct GroupInfo {
    const char* name;      // key in the EvalFile
    int         size;      // number of mg/eg pairs
    const char* category;  // row in the eval breakdown
    const char* index;     // what the table is indexed by, for the EvalFile comments
};

constexpr GroupInfo Groups[GROUP_NB] = {
    {"mobility_knight", 9, "Mobility", "safe squares attacked, 0-8"},
    {"mobility_bishop", 14, "Mobility", "safe squares attacked, 0-13"},
    {"mobility_rook", 15, "Mobility", "safe squares attacked, 0-14"},
    {"mobility_queen", 28, "Mobility", "safe squares attacked, 0-27"},
    {"passed_pawn", 8, "Passed pawns", "rank, from its own side (1-8)"},
    {"passed_blocked", 8, "Passed pawns", "rank; extra when the square ahead is occupied"},
    {"passed_own_king", 8, "Passed pawns", "own king's distance to the square ahead, 0-7"},
    {"passed_their_king", 8, "Passed pawns", "enemy king's distance to the square ahead, 0-7"},
    {"doubled_pawn", 1, "Pawn structure", "per pawn with another own pawn ahead on its file"},
    {"isolated_pawn", 1, "Pawn structure", "per pawn with no own pawn on adjacent files"},
    {"backward_pawn", 1, "Pawn structure", "per pawn that can't be supported and can't advance safely"},
    {"connected_pawn", 8, "Pawn structure", "rank; pawn defended by or side by side with another"},
    {"bishop_pair", 1, "Pieces", "two or more bishops"},
    {"rook_open_file", 1, "Pieces", "per rook on a file without pawns"},
    {"rook_semiopen_file", 1, "Pieces", "per rook on a file with only enemy pawns"},
    {"knight_outpost", 1, "Pieces", "pawn-defended knight no enemy pawn can chase away"},
    {"bishop_outpost", 1, "Pieces", "same for a bishop"},
    {"king_attack", 25, "King safety", "attack units on the enemy king zone, 0-24"},
    {"pawn_shield", 4, "King safety", "own pawns just in front of the king, 0-3"},
    {"king_open_files", 4, "King safety", "files at the king without own pawns, 0-3"},
    {"threat_by_pawn", 6, "Threats", "enemy piece attacked by a pawn, by its type"},
    {"threat_by_minor", 6, "Threats", "enemy piece attacked by a knight or bishop"},
    {"threat_by_rook", 6, "Threats", "enemy piece attacked by a rook"},
    {"hanging", 6, "Threats", "enemy piece attacked and not defended"},
    {"tempo", 1, "Tempo", "side to move"},
};

constexpr int group_offset(int g) {
    int offset = 0;
    for (int i = 0; i < g; ++i)
        offset += Groups[i].size;
    return offset;
}

constexpr int TERM_NB = group_offset(GROUP_NB);

// The same, as a table for use at run time.
constexpr auto GroupOffset = [] {
    std::array<int, GROUP_NB + 1> o{};
    for (int g = 0; g <= GROUP_NB; ++g)
        o[g] = group_offset(g);
    return o;
}();

constexpr const char* Categories[] = {"Mobility", "Passed pawns", "Pawn structure", "Pieces",
                                      "King safety", "Threats", "Tempo"};

// The weights of one evaluation: piece values, placement tables laid out like MgTable
// (white's side, rank 8 first), and the classical terms.
struct Params {
    Score value[PIECE_TYPE_NB];
    int   mg[PIECE_TYPE_NB][64];
    int   eg[PIECE_TYPE_NB][64];
    Score terms[TERM_NB];
    bool  useTerms = false;
};

Params        Pesto, Learned;
std::string   LearnedFile;  // where Learned's weights came from; empty: material only
const Params* Active     = &Pesto;
Eval::Kind    ActiveKind = Eval::Kind::PeSTO;

// Where the learned line starts: textbook piece values, no idea where pieces belong.
Params material_only() {
    Params    p{};
    const int values[PIECE_TYPE_NB] = {100, 300, 300, 500, 900, 0};
    for (int pt = PAWN; pt <= KING; ++pt)
        p.value[pt] = {values[pt], values[pt]};
    p.useTerms = true;
    return p;
}

// Tables are written rank 8 first, so a white piece on square s reads row-mirrored
// entry s ^ 56; a black piece reads entry s directly (the board seen from its side).
int table_index(Piece pc, Square s) { return color_of(pc) == WHITE ? s ^ 56 : s; }

Score placement(Piece pc, Square s) {
    const int i = table_index(pc, s);
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

// Pawn-structure masks, from each side's point of view.
Bitboard ForwardRanks[COLOR_NB][8];     // ranks strictly ahead of a rank
Bitboard AdjacentFiles[8];
Bitboard PassedSpan[COLOR_NB][SQUARE_NB];  // own and adjacent files, ahead
Bitboard AttackSpan[COLOR_NB][SQUARE_NB];  // adjacent files ahead: where enemy pawns could
                                           // come from to attack this square

void init_masks() {
    for (int r = 0; r < 8; ++r) {
        ForwardRanks[WHITE][r] = ForwardRanks[BLACK][r] = 0;
        for (int r2 = 0; r2 < 8; ++r2) {
            if (r2 > r) ForwardRanks[WHITE][r] |= RANK_1_BB << (8 * r2);
            if (r2 < r) ForwardRanks[BLACK][r] |= RANK_1_BB << (8 * r2);
        }
    }
    for (int f = 0; f < 8; ++f)
        AdjacentFiles[f] = (f > 0 ? FILE_A_BB << (f - 1) : 0) | (f < 7 ? FILE_A_BB << (f + 1) : 0);
    for (Color c : {WHITE, BLACK})
        for (Square s = A1; s <= H8; ++s) {
            AttackSpan[c][s] = ForwardRanks[c][rank_of(s)] & AdjacentFiles[file_of(s)];
            PassedSpan[c][s] = AttackSpan[c][s] | (ForwardRanks[c][rank_of(s)] & file_bb(s));
        }
}

int distance(Square a, Square b) {
    return std::max(std::abs(file_of(a) - file_of(b)), std::abs(rank_of(a) - rank_of(b)));
}

Bitboard pawn_attacks_bb(Color c, Bitboard pawns) {
    return c == WHITE ? shift<NORTH_EAST>(pawns) | shift<NORTH_WEST>(pawns)
                      : shift<SOUTH_EAST>(pawns) | shift<SOUTH_WEST>(pawns);
}

constexpr int KingAttackWeight[PIECE_TYPE_NB] = {0, 2, 2, 3, 5, 0};

// Finds every classical term in the position and hands it to `out` as
// out.add(group, index, color, count): `color` gets the term `count` times. Used both to
// evaluate (with weights) and to list features for the tuner (counts only).
template<typename Sink>
void evaluate_terms(const Position& pos, Sink& out) {
    const Bitboard occ = pos.pieces();
    Bitboard pawnAtt[COLOR_NB], attacked[COLOR_NB], minorAtt[COLOR_NB] = {}, rookAtt[COLOR_NB] = {};
    for (Color c : {WHITE, BLACK}) {
        pawnAtt[c]  = pawn_attacks_bb(c, pos.pieces(c, PAWN));
        attacked[c] = pawnAtt[c] | KingAttacks[pos.king_square(c)];
    }

    int kingUnits[COLOR_NB] = {};  // attack units against each side's king
    for (Color c : {WHITE, BLACK}) {
        const Color    them      = ~c;
        const Bitboard mobArea   = ~pos.pieces(c) & ~pawnAtt[them];
        const Square   theirKing = pos.king_square(them);
        const Bitboard zone      = KingAttacks[theirKing] | square_bb(theirKing);
        const Bitboard ownPawns = pos.pieces(c, PAWN), theirPawns = pos.pieces(them, PAWN);

        for (PieceType pt : {KNIGHT, BISHOP, ROOK, QUEEN}) {
            for (Bitboard b = pos.pieces(c, pt); b;) {
                const Square   s = pop_lsb(b);
                const Bitboard a = attacks_bb(pt, s, occ);
                attacked[c] |= a;
                if (pt == KNIGHT || pt == BISHOP)
                    minorAtt[c] |= a;
                if (pt == ROOK)
                    rookAtt[c] |= a;

                out.add(MOBILITY_KNIGHT + pt - KNIGHT, popcount(a & mobArea), c);
                if (a & zone)
                    kingUnits[them] += KingAttackWeight[pt];

                if (pt == ROOK && !(file_bb(s) & ownPawns))
                    out.add(file_bb(s) & theirPawns ? ROOK_SEMIOPEN_FILE : ROOK_OPEN_FILE, 0, c);

                const int r = relative_rank(c, rank_of(s));
                if ((pt == KNIGHT || pt == BISHOP) && r >= 3 && r <= 5
                    && (pawnAtt[c] & square_bb(s)) && !(AttackSpan[c][s] & theirPawns))
                    out.add(pt == KNIGHT ? KNIGHT_OUTPOST : BISHOP_OUTPOST, 0, c);
            }
        }
        if (popcount(pos.pieces(c, BISHOP)) >= 2)
            out.add(BISHOP_PAIR, 0, c);
    }

    for (Color c : {WHITE, BLACK}) {
        const Color    them       = ~c;
        const Bitboard ownPawns   = pos.pieces(c, PAWN);
        const Bitboard theirPawns = pos.pieces(them, PAWN);
        const Square   ownKing    = pos.king_square(c);

        // King safety: c attacks the enemy king zone; c's own pawn cover.
        const Square   theirKing = pos.king_square(them);
        const Bitboard zone      = KingAttacks[theirKing] | square_bb(theirKing);
        const int      units     = kingUnits[them] + popcount(attacked[c] & zone);
        out.add(KING_ATTACK, std::min(units, 24), c);

        int shield = 0, openFiles = 0;
        for (int f = std::max(file_of(ownKing) - 1, 0); f <= std::min(file_of(ownKing) + 1, 7); ++f) {
            const Bitboard file = FILE_A_BB << f;
            if (!(file & ownPawns))
                ++openFiles;
            const int      kr    = rank_of(ownKing);
            const Bitboard front = c == WHITE ? (kr < 7 ? RANK_1_BB << (8 * (kr + 1)) : 0)
                                                    | (kr < 6 ? RANK_1_BB << (8 * (kr + 2)) : 0)
                                              : (kr > 0 ? RANK_1_BB << (8 * (kr - 1)) : 0)
                                                    | (kr > 1 ? RANK_1_BB << (8 * (kr - 2)) : 0);
            shield += bool(file & front & ownPawns);
        }
        out.add(PAWN_SHIELD, shield, c);
        out.add(KING_OPEN_FILES, openFiles, c);

        // Pawn structure.
        for (Bitboard b = ownPawns; b;) {
            const Square s    = pop_lsb(b);
            const int    f    = file_of(s);
            const int    r    = relative_rank(c, rank_of(s));
            const Square stop = s + pawn_push(c);

            const bool doubled  = ForwardRanks[c][rank_of(s)] & file_bb(s) & ownPawns;
            const bool isolated = !(AdjacentFiles[f] & ownPawns);
            if (doubled)
                out.add(DOUBLED_PAWN, 0, c);
            if (isolated)
                out.add(ISOLATED_PAWN, 0, c);
            else if (!(AdjacentFiles[f] & ownPawns & ~ForwardRanks[c][rank_of(s)])
                     && (pawnAtt[them] & square_bb(stop)))
                out.add(BACKWARD_PAWN, 0, c);

            const bool phalanx   = AdjacentFiles[f] & rank_bb(s) & ownPawns;
            const bool supported = pawn_attacks(them, s) & ownPawns;
            if (phalanx || supported)
                out.add(CONNECTED_PAWN, r, c);

            if (!(PassedSpan[c][s] & theirPawns) && !doubled) {
                out.add(PASSED_PAWN, r, c);
                if (pos.pieces() & square_bb(stop))
                    out.add(PASSED_BLOCKED, r, c);
                out.add(PASSED_OWN_KING, std::min(distance(ownKing, stop), 7), c);
                out.add(PASSED_THEIR_KING, std::min(distance(theirKing, stop), 7), c);
            }
        }

        // Threats against c's pieces, credited to the attacker.
        for (PieceType pt : {PAWN, KNIGHT, BISHOP, ROOK, QUEEN}) {
            const Bitboard ours = pos.pieces(c, pt);
            if (pt != PAWN)
                if (const int n = popcount(ours & pawnAtt[them]))
                    out.add(THREAT_BY_PAWN, pt, them, n);
            if (const int n = popcount(ours & minorAtt[them]))
                out.add(THREAT_BY_MINOR, pt, them, n);
            if (const int n = popcount(ours & rookAtt[them]))
                out.add(THREAT_BY_ROOK, pt, them, n);
            if (const int n = popcount(ours & attacked[them] & ~attacked[c]))
                out.add(HANGING, pt, them, n);
        }
    }

    out.add(TEMPO, 0, pos.side_to_move());
}

// Sums the active weights; also per group, for the breakdown.
struct ScoreSink {
    const Score* w;
    Score        total[COLOR_NB]           = {};
    Score        byGroup[GROUP_NB][COLOR_NB] = {};
    bool         keepGroups                = false;

    void add(int g, int idx, Color c, int n = 1) {
        const Score s = w[GroupOffset[g] + idx];
        const Score v = {s.mg * n, s.eg * n};
        total[c] += v;
        if (keepGroups)
            byGroup[g][c] += v;
    }
};

Score terms_score(const Position& pos) {
    ScoreSink sink{Active->terms};
    evaluate_terms(pos, sink);
    return sink.total[WHITE] + -sink.total[BLACK];
}

// Counts, white minus black, for the tuner.
struct FeatureSink {
    int coeff[TERM_NB] = {};
    void add(int g, int idx, Color c, int n = 1) { coeff[GroupOffset[g] + idx] += c == WHITE ? n : -n; }
};

}  // namespace

void Eval::init() {
    init_masks();
    std::copy(std::begin(PestoValue), std::end(PestoValue), Pesto.value);
    std::copy(&MgTable[0][0], &MgTable[0][0] + PIECE_TYPE_NB * 64, &Pesto.mg[0][0]);
    std::copy(&EgTable[0][0], &EgTable[0][0] + PIECE_TYPE_NB * 64, &Pesto.eg[0][0]);
    Learned = material_only();
    use(Kind::PeSTO);
}

void Eval::use(Kind kind) {
    ActiveKind    = kind;
    Active        = kind == Kind::PeSTO ? &Pesto : &Learned;  // NNUE: tables only for `eval`
    NNUE::enabled = kind == Kind::NNUE;
    rebuild_piece_square();
}

Eval::Kind Eval::active() { return ActiveKind; }

std::string Eval::learned_file() { return LearnedFile; }

bool Eval::load_learned(const std::string& path, std::string& error) {
    if (path.empty()) {
        Learned = material_only();
        LearnedFile.clear();
    } else {
        std::ifstream in(path);
        if (!in) {
            error = "can't open " + path;
            return false;
        }
        if (!load_learned(in, error))
            return false;
        LearnedFile = path;
    }
    if (ActiveKind == Kind::Learned)
        rebuild_piece_square();
    return true;
}

// Format: "value <piece> <mg> <eg>", "mg <piece> <64 numbers>", "eg <piece> <64 numbers>",
// "<term> <mg> <eg> [<mg> <eg> ...]" for the classical terms, in any order and layout;
// '#' starts a comment. Anything not given stays material only (terms zero).
bool Eval::load_learned(std::istream& in, std::string& error) {
    std::string text, line;
    while (std::getline(in, line))
        text += line.substr(0, line.find('#')) + '\n';

    std::istringstream is(text);
    Params             p = material_only();
    std::string        key, piece;
    while (is >> key) {
        const auto group = std::find_if(std::begin(Groups), std::end(Groups),
                                        [&](const GroupInfo& g) { return key == g.name; });
        if (group != std::end(Groups)) {
            Score* w = p.terms + GroupOffset[int(group - std::begin(Groups))];
            for (int i = 0; i < group->size; ++i)
                if (!(is >> w[i].mg >> w[i].eg)) {
                    error = "expected " + std::to_string(group->size) + " mg/eg pairs for " + key;
                    return false;
                }
            continue;
        }

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

namespace {

std::string params_text(const Params& p, const std::string& name) {
    std::ostringstream os;
    os << "# " << name << " evaluation weights (centipawns). Placement tables are from White's side,\n"
       << "# drawn like a board: the first row is rank 8.\n";
    for (int pt = PAWN; pt <= KING; ++pt) {
        os << "\nvalue " << PieceNames[pt] << ' ' << p.value[pt].mg << ' ' << p.value[pt].eg << '\n';
        for (const char* key : {"mg", "eg"}) {
            const int* table = std::string(key) == "mg" ? p.mg[pt] : p.eg[pt];
            os << key << ' ' << PieceNames[pt] << '\n';
            for (int i = 0; i < 64; ++i) {
                char buf[8];
                std::snprintf(buf, sizeof buf, "%5d", table[i]);
                os << buf << (i % 8 == 7 ? "\n" : "");
            }
        }
    }
    if (!p.useTerms)
        return os.str();

    os << "\n# Classical terms: mg/eg pairs, one per table entry.\n";
    for (int g = 0; g < GROUP_NB; ++g) {
        os << "\n# " << Groups[g].index << '\n' << Groups[g].name;
        const Score* w = p.terms + GroupOffset[g];
        for (int i = 0; i < Groups[g].size; ++i)
            os << (i % 8 == 0 ? "\n " : "") << ' ' << w[i].mg << ' ' << w[i].eg << (i % 8 == 7 ? "" : " ");
        os << '\n';
    }
    return os.str();
}

}  // namespace

std::string Eval::params() {
    if (ActiveKind == Kind::NNUE)
        return "# NNUE network " + NNUE::file() + ": its weights are in that binary file.\n";
    return params_text(*Active, ActiveKind == Kind::PeSTO ? "PeSTO" : "Learned");
}

int Eval::evaluate(const Position& pos) {
    if (NNUE::enabled)
        return NNUE::evaluate(pos.accumulator(), pos.side_to_move());
    Score s = pos.psq_score();
    if (Active->useTerms)
        s += terms_score(pos);
    const int v = taper(s, pos.phase());
    return pos.side_to_move() == WHITE ? v : -v;
}

std::string Eval::format_score(int cp) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "%+.2f", cp / 100.0);
    return buf;
}

namespace {

// A network has no named terms to show. Instead: what each piece is worth where it
// stands, as the evaluation's drop (from its owner's side) if it were taken off.
std::string nnue_trace(const Position& pos) {
    const Color stm   = pos.side_to_move();
    const int   white = (stm == WHITE ? 1 : -1) * NNUE::evaluate(pos.accumulator(), stm);
    std::ostringstream os;
    os << "Evaluation function: NNUE (" << NNUE::file() << ")\n\n"
       << "Each piece's worth here, in pawns (evaluation drop without it):\n\n";
    for (int r = 7; r >= 0; --r) {
        os << ' ' << r + 1 << ' ';
        for (int f = 0; f < 8; ++f) {
            const Square s  = make_square(f, r);
            const Piece  pc = pos.piece_on(s);
            if (pc == NO_PIECE || type_of(pc) == KING) {
                os << (pc == NO_PIECE ? "      ." : (color_of(pc) == WHITE ? "      K" : "      k"));
                continue;
            }
            NNUE::Accumulator without = pos.accumulator();
            NNUE::remove(without, pc, s);
            const int whiteWithout = (stm == WHITE ? 1 : -1) * NNUE::evaluate(without, stm);
            const int worth        = (color_of(pc) == WHITE ? 1 : -1) * (white - whiteWithout);
            char      buf[16];
            std::snprintf(buf, sizeof buf, " %c%5.1f", "PNBRQKpnbrqk"[pc], worth / 100.0);
            os << buf;
        }
        os << '\n';
    }
    os << "        a      b      c      d      e      f      g      h\n\n"
       << "Evaluation: " << Eval::format_score(white) << " (+ favors White)\n";
    return os.str();
}

}  // namespace

std::string Eval::trace(const Position& pos) {
    if (ActiveKind == Kind::NNUE)
        return nnue_trace(pos);

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
        os << pad(label, 15) << pad(std::to_string(w.mg), 7) << pad(std::to_string(w.eg), 7)
           << pad(std::to_string(b.mg), 9) << pad(std::to_string(b.eg), 7)
           << pad(std::to_string(d.mg), 9) << pad(std::to_string(d.eg), 7)
           << pad(format_score(taper(d, phase)), 10) << '\n';
    };

    os << "                      White            Black       White - Black\n"
       << "                    MG     EG       MG     EG       MG     EG   blended\n";
    row("Material", material[WHITE], material[BLACK]);
    row("Placement", place[WHITE], place[BLACK]);
    Score total[COLOR_NB] = {material[WHITE] + place[WHITE], material[BLACK] + place[BLACK]};

    if (Active->useTerms) {
        ScoreSink sink{Active->terms};
        sink.keepGroups = true;
        evaluate_terms(pos, sink);
        for (const char* cat : Categories) {
            Score cw, cb;
            for (int g = 0; g < GROUP_NB; ++g)
                if (std::string(Groups[g].category) == cat) {
                    cw += sink.byGroup[g][WHITE];
                    cb += sink.byGroup[g][BLACK];
                }
            row(cat, cw, cb);
        }
        total[WHITE] += sink.total[WHITE];
        total[BLACK] += sink.total[BLACK];
    }
    row("Total", total[WHITE], total[BLACK]);

    const int white = taper(total[WHITE] + -total[BLACK], pos.phase());
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

// Tuner interface. Parameter layout: 6 piece values, then 6 x 64 placement entries
// (mg table entry and eg table entry as one pair), then the classical terms.
int Eval::param_count() { return PIECE_TYPE_NB + PIECE_TYPE_NB * 64 + TERM_NB; }

void Eval::features(const Position& pos, std::vector<Feature>& out) {
    out.clear();
    int psq[PIECE_TYPE_NB + PIECE_TYPE_NB * 64] = {};
    for (Square s = A1; s <= H8; ++s) {
        const Piece pc = pos.piece_on(s);
        if (pc == NO_PIECE)
            continue;
        const int sign = color_of(pc) == WHITE ? 1 : -1;
        psq[type_of(pc)] += sign;
        psq[PIECE_TYPE_NB + type_of(pc) * 64 + table_index(pc, s)] += sign;
    }
    for (int i = 0; i < int(std::size(psq)); ++i)
        if (psq[i])
            out.push_back({int16_t(i), int16_t(psq[i])});

    FeatureSink sink;
    evaluate_terms(pos, sink);
    const int base = int(std::size(psq));
    for (int i = 0; i < TERM_NB; ++i)
        if (sink.coeff[i])
            out.push_back({int16_t(base + i), int16_t(sink.coeff[i])});
}

std::vector<Score> Eval::learned_params() {
    std::vector<Score> w(param_count());
    for (int pt = PAWN; pt <= KING; ++pt) {
        w[pt] = Learned.value[pt];
        for (int i = 0; i < 64; ++i)
            w[PIECE_TYPE_NB + pt * 64 + i] = {Learned.mg[pt][i], Learned.eg[pt][i]};
    }
    std::copy(std::begin(Learned.terms), std::end(Learned.terms), w.begin() + PIECE_TYPE_NB * 65);
    return w;
}

std::string Eval::learned_params_text(const std::vector<Score>& w) {
    Params p = material_only();
    for (int pt = PAWN; pt <= KING; ++pt) {
        p.value[pt] = w[pt];
        for (int i = 0; i < 64; ++i) {
            p.mg[pt][i] = w[PIECE_TYPE_NB + pt * 64 + i].mg;
            p.eg[pt][i] = w[PIECE_TYPE_NB + pt * 64 + i].eg;
        }
    }
    std::copy(w.begin() + PIECE_TYPE_NB * 65, w.end(), p.terms);
    return params_text(p, "Learned");
}

void Eval::normalize(std::vector<Score>& w, const std::vector<uint64_t>& seen) {
    auto fold = [&](int pt, int first, int size) {
        Score sum;
        int   n = 0;
        for (int i = first; i < first + size; ++i)
            if (seen[i]) {
                sum += w[i];
                ++n;
            }
        if (!n)
            return;
        const Score mean = {sum.mg / n, sum.eg / n};
        for (int i = first; i < first + size; ++i)
            w[i] -= mean;
        if (pt != KING)  // the king's average cancels out: both sides have one
            w[pt] += mean;
    };
    for (int pt = PAWN; pt <= KING; ++pt)
        fold(pt, PIECE_TYPE_NB + pt * 64, 64);
    for (int pt = KNIGHT; pt <= QUEEN; ++pt) {
        const int g = MOBILITY_KNIGHT + pt - KNIGHT;
        fold(pt, PIECE_TYPE_NB * 65 + GroupOffset[g], Groups[g].size);
    }
}

std::string Eval::param_name(int i) {
    if (i < PIECE_TYPE_NB)
        return std::string("value ") + PieceNames[i];
    if (i < PIECE_TYPE_NB * 65) {
        const int pt = (i - PIECE_TYPE_NB) / 64, sq = (i - PIECE_TYPE_NB) % 64;
        return std::string("placement ") + PieceNames[pt] + ' ' + char('a' + sq % 8)
             + char('8' - sq / 8);
    }
    i -= PIECE_TYPE_NB * 65;
    for (int g = 0; g < GROUP_NB; ++g)
        if (i < GroupOffset[g] + Groups[g].size)
            return std::string(Groups[g].name) + '[' + std::to_string(i - GroupOffset[g]) + ']';
    return "?";
}
