#pragma once

#include <cstdint>
#include <string>

using Bitboard = uint64_t;
using Key      = uint64_t;

enum Color : int { WHITE, BLACK, COLOR_NB = 2 };

constexpr Color operator~(Color c) { return Color(c ^ 1); }

enum PieceType : int { PAWN, KNIGHT, BISHOP, ROOK, QUEEN, KING, PIECE_TYPE_NB = 6 };

// Piece index = color * 6 + type, so 12 pieces x 64 squares = the 768 NNUE inputs later.
enum Piece : int {
    W_PAWN, W_KNIGHT, W_BISHOP, W_ROOK, W_QUEEN, W_KING,
    B_PAWN, B_KNIGHT, B_BISHOP, B_ROOK, B_QUEEN, B_KING,
    NO_PIECE, PIECE_NB = 12
};

constexpr Piece     make_piece(Color c, PieceType pt) { return Piece(c * 6 + pt); }
constexpr Color     color_of(Piece p) { return Color(p / 6); }
constexpr PieceType type_of(Piece p) { return PieceType(p % 6); }

// a1 = 0, b1 = 1, ..., h8 = 63
enum Square : int {
    A1, B1, C1, D1, E1, F1, G1, H1,
    A2, B2, C2, D2, E2, F2, G2, H2,
    A3, B3, C3, D3, E3, F3, G3, H3,
    A4, B4, C4, D4, E4, F4, G4, H4,
    A5, B5, C5, D5, E5, F5, G5, H5,
    A6, B6, C6, D6, E6, F6, G6, H6,
    A7, B7, C7, D7, E7, F7, G7, H7,
    A8, B8, C8, D8, E8, F8, G8, H8,
    NO_SQUARE, SQUARE_NB = 64
};

enum Direction : int {
    NORTH = 8, EAST = 1, SOUTH = -8, WEST = -1,
    NORTH_EAST = 9, NORTH_WEST = 7, SOUTH_EAST = -7, SOUTH_WEST = -9
};

constexpr Square operator+(Square s, int d) { return Square(int(s) + d); }
constexpr Square operator-(Square s, int d) { return Square(int(s) - d); }
inline Square&   operator++(Square& s) { return s = Square(int(s) + 1); }

constexpr int       file_of(Square s) { return s & 7; }
constexpr int       rank_of(Square s) { return s >> 3; }
constexpr Square    make_square(int file, int rank) { return Square(rank * 8 + file); }
constexpr int       relative_rank(Color c, int rank) { return c == WHITE ? rank : 7 - rank; }
constexpr Direction pawn_push(Color c) { return c == WHITE ? NORTH : SOUTH; }

// Castling rights, one bit each.
constexpr int WHITE_OO = 1, WHITE_OOO = 2, BLACK_OO = 4, BLACK_OOO = 8;
constexpr int ALL_CASTLING = 15;

// Move layout: bits 0-5 from, 6-11 to, 12-13 promotion piece (N, B, R, Q), 14-15 type.
// Castling is encoded as the king's move (e1g1), which is also its UCI form.
enum MoveType : int { NORMAL = 0, PROMOTION = 1 << 14, EN_PASSANT = 2 << 14, CASTLING = 3 << 14 };

class Move {
  public:
    Move() = default;
    constexpr explicit Move(uint16_t d) : data(d) {}
    constexpr Move(Square from, Square to, MoveType mt = NORMAL, PieceType promo = KNIGHT)
        : data(uint16_t(from | (to << 6) | mt | ((promo - KNIGHT) << 12))) {}

    static constexpr Move none() { return Move(uint16_t(0)); }

    constexpr Square    from() const { return Square(data & 0x3F); }
    constexpr Square    to() const { return Square((data >> 6) & 0x3F); }
    constexpr MoveType  type() const { return MoveType(data & (3 << 14)); }
    constexpr PieceType promotion_type() const { return PieceType(((data >> 12) & 3) + KNIGHT); }
    constexpr bool      is_ok() const { return from() != to(); }
    constexpr uint16_t  raw() const { return data; }

    constexpr bool operator==(const Move&) const = default;

  private:
    uint16_t data;
};

inline std::string to_string(Square s) {
    return {char('a' + file_of(s)), char('1' + rank_of(s))};
}

inline std::string to_uci(Move m) {
    if (!m.is_ok())
        return "0000";
    std::string s = to_string(m.from()) + to_string(m.to());
    if (m.type() == PROMOTION)
        s += "nbrq"[m.promotion_type() - KNIGHT];
    return s;
}
