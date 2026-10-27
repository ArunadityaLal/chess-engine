#include "bitboard.h"

#include "prng.h"

Bitboard PawnAttacks[COLOR_NB][SQUARE_NB];
Bitboard KnightAttacks[SQUARE_NB];
Bitboard KingAttacks[SQUARE_NB];
Magic    BishopMagics[SQUARE_NB];
Magic    RookMagics[SQUARE_NB];

namespace {

Bitboard BishopTable[0x1480];  // sum over squares of 2^(mask bits) = 5248
Bitboard RookTable[0x19000];   // = 102400

// The square (file + df, rank + dr) as a bitboard, or 0 if that's off the board.
Bitboard step(Square s, int df, int dr) {
    const int f = file_of(s) + df, r = rank_of(s) + dr;
    return (f >= 0 && f < 8 && r >= 0 && r < 8) ? square_bb(make_square(f, r)) : 0;
}

void init_magics(PieceType pt, Bitboard* table, Magic* magics) {
    // Seeds per rank that find magics quickly (from Stockfish); any seed works, just slower.
    constexpr uint64_t Seeds[8] = {728, 10316, 55013, 32803, 12281, 15100, 16645, 255};

    Bitboard  occupancy[4096], reference[4096];
    int       epoch[4096] = {}, attempt = 0;
    Bitboard* next = table;

    for (Square s = A1; s <= H8; ++s) {
        // Edge squares never block anything beyond them, so leave them out of the mask
        // (unless the slider sits on that edge).
        const Bitboard edges = ((RANK_1_BB | RANK_8_BB) & ~rank_bb(s))
                             | ((FILE_A_BB | FILE_H_BB) & ~file_bb(s));
        Magic& m  = magics[s];
        m.mask    = slider_attacks_slow(pt, s, 0) & ~edges;
        m.shift   = 64 - popcount(m.mask);
        m.attacks = next;

        // Enumerate every subset of the mask (carry-rippler trick).
        int      size = 0;
        Bitboard b    = 0;
        do {
            occupancy[size] = b;
            reference[size] = slider_attacks_slow(pt, s, b);
            ++size;
            b = (b - m.mask) & m.mask;
        } while (b);
        next += size;

        // Try random magics until every occupancy maps to a slot with the right attacks.
        // epoch[] marks which slots were written by the current attempt, avoiding a clear.
        PRNG rng(Seeds[rank_of(s)]);
        for (int i = 0; i < size;) {
            do
                m.magic = rng.sparse();
            while (popcount((m.mask * m.magic) >> 56) < 6);

            ++attempt;
            for (i = 0; i < size; ++i) {
                const unsigned idx = m.index(occupancy[i]);
                if (epoch[idx] < attempt) {
                    epoch[idx]     = attempt;
                    m.attacks[idx] = reference[i];
                } else if (m.attacks[idx] != reference[i])
                    break;
            }
        }
    }
}

}  // namespace

Bitboard slider_attacks_slow(PieceType pt, Square s, Bitboard occ) {
    static constexpr int RookDirs[4][2]   = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
    static constexpr int BishopDirs[4][2] = {{1, 1}, {1, -1}, {-1, 1}, {-1, -1}};
    const int (*dirs)[2] = pt == ROOK ? RookDirs : BishopDirs;

    Bitboard attacks = 0;
    for (int d = 0; d < 4; ++d) {
        int f = file_of(s) + dirs[d][0], r = rank_of(s) + dirs[d][1];
        while (f >= 0 && f < 8 && r >= 0 && r < 8) {
            const Bitboard b = square_bb(make_square(f, r));
            attacks |= b;
            if (occ & b)
                break;
            f += dirs[d][0];
            r += dirs[d][1];
        }
    }
    return attacks;
}

void Bitboards::init() {
    for (Square s = A1; s <= H8; ++s) {
        PawnAttacks[WHITE][s] = step(s, -1, 1) | step(s, 1, 1);
        PawnAttacks[BLACK][s] = step(s, -1, -1) | step(s, 1, -1);

        KnightAttacks[s] = step(s, 1, 2) | step(s, 2, 1) | step(s, 2, -1) | step(s, 1, -2)
                         | step(s, -1, -2) | step(s, -2, -1) | step(s, -2, 1) | step(s, -1, 2);

        KingAttacks[s] = step(s, -1, -1) | step(s, 0, -1) | step(s, 1, -1) | step(s, -1, 0)
                       | step(s, 1, 0) | step(s, -1, 1) | step(s, 0, 1) | step(s, 1, 1);
    }

    init_magics(BISHOP, BishopTable, BishopMagics);
    init_magics(ROOK, RookTable, RookMagics);
}

bool Bitboards::verify_magics() {
    PRNG rng(42);
    for (Square s = A1; s <= H8; ++s)
        for (int i = 0; i < 3000; ++i) {
            // Mix dense, medium and sparse boards.
            const Bitboard occ = i % 3 == 0 ? rng.rand()
                               : i % 3 == 1 ? rng.rand() & rng.rand()
                                            : rng.sparse();
            if (bishop_attacks(s, occ) != slider_attacks_slow(BISHOP, s, occ)
                || rook_attacks(s, occ) != slider_attacks_slow(ROOK, s, occ))
                return false;
        }
    return true;
}

std::string Bitboards::pretty(Bitboard b) {
    std::string s;
    for (int r = 7; r >= 0; --r) {
        for (int f = 0; f < 8; ++f)
            s += (b & square_bb(make_square(f, r))) ? "X " : ". ";
        s += '\n';
    }
    return s;
}
