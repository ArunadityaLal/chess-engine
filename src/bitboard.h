#pragma once

#include <bit>
#include <cassert>
#include <string>

#include "types.h"

constexpr Bitboard FILE_A_BB = 0x0101010101010101ULL;
constexpr Bitboard FILE_H_BB = FILE_A_BB << 7;
constexpr Bitboard RANK_1_BB = 0xFFULL;
constexpr Bitboard RANK_3_BB = RANK_1_BB << 16;
constexpr Bitboard RANK_6_BB = RANK_1_BB << 40;
constexpr Bitboard RANK_8_BB = RANK_1_BB << 56;

constexpr Bitboard square_bb(Square s) { return 1ULL << s; }
constexpr Bitboard file_bb(Square s) { return FILE_A_BB << file_of(s); }
constexpr Bitboard rank_bb(Square s) { return RANK_1_BB << (8 * rank_of(s)); }

inline int popcount(Bitboard b) { return std::popcount(b); }

inline Square lsb(Bitboard b) {
    assert(b);
    return Square(std::countr_zero(b));
}

inline Square pop_lsb(Bitboard& b) {
    const Square s = lsb(b);
    b &= b - 1;
    return s;
}

// Shift every square one step in direction D, dropping squares that would wrap a file.
template<Direction D>
constexpr Bitboard shift(Bitboard b) {
    if constexpr (D == NORTH)      return b << 8;
    else if constexpr (D == SOUTH) return b >> 8;
    else if constexpr (D == EAST)  return (b & ~FILE_H_BB) << 1;
    else if constexpr (D == WEST)  return (b & ~FILE_A_BB) >> 1;
    else if constexpr (D == NORTH_EAST) return (b & ~FILE_H_BB) << 9;
    else if constexpr (D == NORTH_WEST) return (b & ~FILE_A_BB) << 7;
    else if constexpr (D == SOUTH_EAST) return (b & ~FILE_H_BB) >> 7;
    else                                return (b & ~FILE_A_BB) >> 9;
}

// Fancy magic bitboards: attacks = table[((occupancy & mask) * magic) >> shift]
struct Magic {
    Bitboard  mask;
    Bitboard  magic;
    Bitboard* attacks;
    unsigned  shift;

    unsigned index(Bitboard occ) const { return unsigned(((occ & mask) * magic) >> shift); }
};

extern Bitboard PawnAttacks[COLOR_NB][SQUARE_NB];
extern Bitboard KnightAttacks[SQUARE_NB];
extern Bitboard KingAttacks[SQUARE_NB];
extern Magic    BishopMagics[SQUARE_NB];
extern Magic    RookMagics[SQUARE_NB];

namespace Bitboards {
void        init();
bool        verify_magics();  // magic lookups == slow ray-walk on random occupancies
std::string pretty(Bitboard b);
}  // namespace Bitboards

// Reference implementation: walks rays square by square. Used to build and verify magics.
Bitboard slider_attacks_slow(PieceType pt, Square s, Bitboard occ);

inline Bitboard pawn_attacks(Color c, Square s) { return PawnAttacks[c][s]; }

inline Bitboard bishop_attacks(Square s, Bitboard occ) {
    const Magic& m = BishopMagics[s];
    return m.attacks[m.index(occ)];
}

inline Bitboard rook_attacks(Square s, Bitboard occ) {
    const Magic& m = RookMagics[s];
    return m.attacks[m.index(occ)];
}

inline Bitboard attacks_bb(PieceType pt, Square s, Bitboard occ) {
    switch (pt) {
    case KNIGHT: return KnightAttacks[s];
    case BISHOP: return bishop_attacks(s, occ);
    case ROOK:   return rook_attacks(s, occ);
    case QUEEN:  return bishop_attacks(s, occ) | rook_attacks(s, occ);
    case KING:   return KingAttacks[s];
    default:     assert(false); return 0;
    }
}
