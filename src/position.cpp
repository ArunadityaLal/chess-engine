#include "position.h"

#include <algorithm>
#include <cassert>
#include <iomanip>
#include <sstream>
#include <string_view>
#include <utility>

#include "prng.h"

namespace Zobrist {
Key psq[PIECE_NB][SQUARE_NB];
Key enpassant[8];
Key castling[16];
Key side;
}  // namespace Zobrist

namespace {

constexpr std::string_view PieceChars = "PNBRQKpnbrqk";

// rights &= CastlingMask[from] & CastlingMask[to]: moving a king or rook off its home
// square, or capturing a rook on it, removes the matching rights.
int CastlingMask[SQUARE_NB];

// Rook from/to squares for a castling move given as the king's move.
std::pair<Square, Square> castling_rook_squares(Square kfrom, Square kto) {
    return kto > kfrom ? std::pair{kfrom + 3, kfrom + 1}   // O-O:   h-file rook to f-file
                       : std::pair{kfrom - 4, kfrom - 1};  // O-O-O: a-file rook to d-file
}

}  // namespace

void Position::init() {
    PRNG rng(1070372);
    for (auto& pieceKeys : Zobrist::psq)
        for (Key& k : pieceKeys)
            k = rng.rand();
    for (Key& k : Zobrist::enpassant)
        k = rng.rand();
    for (Key& k : Zobrist::castling)
        k = rng.rand();
    Zobrist::side = rng.rand();

    std::fill(std::begin(CastlingMask), std::end(CastlingMask), ALL_CASTLING);
    CastlingMask[E1] &= ~(WHITE_OO | WHITE_OOO);
    CastlingMask[H1] &= ~WHITE_OO;
    CastlingMask[A1] &= ~WHITE_OOO;
    CastlingMask[E8] &= ~(BLACK_OO | BLACK_OOO);
    CastlingMask[H8] &= ~BLACK_OO;
    CastlingMask[A8] &= ~BLACK_OOO;
}

Position::Position() {
    history.reserve(1024);
    parse(StartFEN);
}

bool Position::set(const std::string& fen) {
    const Position backup = *this;
    if (parse(fen))
        return true;
    *this = backup;
    return false;
}

void Position::clear() {
    std::fill(std::begin(board), std::end(board), NO_PIECE);
    std::fill(std::begin(byTypeBB), std::end(byTypeBB), 0);
    std::fill(std::begin(byColorBB), std::end(byColorBB), 0);
    sideToMove = WHITE;
    gamePly    = 0;
    psqScore   = {};
    gamePhase  = 0;
    history.clear();
}

bool Position::parse(const std::string& fen) {
    clear();

    std::istringstream is(fen);
    std::string        placement, side, castling = "-", ep = "-";
    int                halfmove = 0, fullmove = 1;
    if (!(is >> placement >> side))
        return false;
    is >> castling >> ep;
    if (!(is >> halfmove))
        halfmove = 0;
    if (!(is >> fullmove))
        fullmove = 1;

    int file = 0, rank = 7;
    for (char c : placement) {
        if (c == '/') {
            if (file != 8 || rank == 0)
                return false;
            file = 0;
            --rank;
        } else if (c >= '1' && c <= '8') {
            file += c - '0';
            if (file > 8)
                return false;
        } else {
            const size_t idx = PieceChars.find(c);
            if (idx == std::string_view::npos || file > 7)
                return false;
            add_piece(Piece(idx), make_square(file, rank));
            ++file;
        }
    }
    if (rank != 0 || file != 8)
        return false;
    if (popcount(pieces(WHITE, KING)) != 1 || popcount(pieces(BLACK, KING)) != 1)
        return false;

    if (side == "w")
        sideToMove = WHITE;
    else if (side == "b")
        sideToMove = BLACK;
    else
        return false;

    StateInfo s{};
    s.ep       = NO_SQUARE;
    s.captured = NO_PIECE;
    s.halfmove = halfmove;

    for (char c : castling) {
        switch (c) {
        case 'K': s.castling |= WHITE_OO; break;
        case 'Q': s.castling |= WHITE_OOO; break;
        case 'k': s.castling |= BLACK_OO; break;
        case 'q': s.castling |= BLACK_OOO; break;
        case '-': break;
        default:  return false;
        }
    }
    // Drop rights whose king or rook isn't on its home square, so movegen can trust them.
    if (piece_on(E1) != W_KING) s.castling &= ~(WHITE_OO | WHITE_OOO);
    if (piece_on(H1) != W_ROOK) s.castling &= ~WHITE_OO;
    if (piece_on(A1) != W_ROOK) s.castling &= ~WHITE_OOO;
    if (piece_on(E8) != B_KING) s.castling &= ~(BLACK_OO | BLACK_OOO);
    if (piece_on(H8) != B_ROOK) s.castling &= ~BLACK_OO;
    if (piece_on(A8) != B_ROOK) s.castling &= ~BLACK_OOO;

    if (ep != "-") {
        if (ep.size() != 2 || ep[0] < 'a' || ep[0] > 'h' || ep[1] < '1' || ep[1] > '8')
            return false;
        const Square sq = make_square(ep[0] - 'a', ep[1] - '1');
        // Keep it only if a pawn can actually capture there, so equal positions hash equally.
        if (relative_rank(sideToMove, rank_of(sq)) == 5
            && (pawn_attacks(~sideToMove, sq) & pieces(sideToMove, PAWN)))
            s.ep = sq;
    }

    gamePly = std::max(2 * (fullmove - 1), 0) + (sideToMove == BLACK);
    history.push_back(s);
    st().key = compute_key();

    // The side not to move can't be in check: that would mean its king can be captured.
    return !mover_in_check();
}

std::string Position::fen() const {
    std::string s;
    for (int r = 7; r >= 0; --r) {
        int empty = 0;
        for (int f = 0; f < 8; ++f) {
            const Piece p = piece_on(make_square(f, r));
            if (p == NO_PIECE) {
                ++empty;
                continue;
            }
            if (empty)
                s += char('0' + empty);
            empty = 0;
            s += PieceChars[p];
        }
        if (empty)
            s += char('0' + empty);
        if (r)
            s += '/';
    }

    s += sideToMove == WHITE ? " w " : " b ";

    const int cr = castling_rights();
    if (cr & WHITE_OO)  s += 'K';
    if (cr & WHITE_OOO) s += 'Q';
    if (cr & BLACK_OO)  s += 'k';
    if (cr & BLACK_OOO) s += 'q';
    if (!cr)            s += '-';

    s += ' ';
    s += ep_square() == NO_SQUARE ? "-" : to_string(ep_square());
    s += ' ' + std::to_string(halfmove_clock()) + ' ' + std::to_string(gamePly / 2 + 1);
    return s;
}

std::string Position::pretty() const {
    std::ostringstream os;
    for (int r = 7; r >= 0; --r) {
        os << ' ' << r + 1 << "  ";
        for (int f = 0; f < 8; ++f) {
            const Piece p = piece_on(make_square(f, r));
            os << (p == NO_PIECE ? '.' : PieceChars[p]) << ' ';
        }
        os << '\n';
    }
    os << "\n    a b c d e f g h\n\n"
       << "FEN: " << fen() << '\n'
       << "Key: " << std::hex << std::setw(16) << std::setfill('0') << key() << std::dec << '\n';
    return os.str();
}

void Position::add_piece(Piece p, Square s) {
    const Bitboard b = square_bb(s);
    board[s] = p;
    byTypeBB[type_of(p)] |= b;
    byColorBB[color_of(p)] |= b;
    psqScore += Eval::PieceSquare[p][s];
    gamePhase += Eval::PhaseWeight[type_of(p)];
}

void Position::remove_piece(Square s) {
    const Piece    p = board[s];
    const Bitboard b = square_bb(s);
    byTypeBB[type_of(p)] ^= b;
    byColorBB[color_of(p)] ^= b;
    board[s] = NO_PIECE;
    psqScore -= Eval::PieceSquare[p][s];
    gamePhase -= Eval::PhaseWeight[type_of(p)];
}

void Position::move_piece(Square from, Square to) {
    const Piece    p      = board[from];
    const Bitboard fromTo = square_bb(from) | square_bb(to);
    byTypeBB[type_of(p)] ^= fromTo;
    byColorBB[color_of(p)] ^= fromTo;
    board[from] = NO_PIECE;
    board[to]   = p;
    psqScore += Eval::PieceSquare[p][to];
    psqScore -= Eval::PieceSquare[p][from];
}

void Position::refresh_eval() {
    psqScore  = {};
    gamePhase = 0;
    for (Square s = A1; s <= H8; ++s)
        if (board[s] != NO_PIECE) {
            psqScore += Eval::PieceSquare[board[s]][s];
            gamePhase += Eval::PhaseWeight[type_of(board[s])];
        }
}

Bitboard Position::attackers_to(Square s, Bitboard occ) const {
    return (pawn_attacks(BLACK, s) & pieces(WHITE, PAWN))
         | (pawn_attacks(WHITE, s) & pieces(BLACK, PAWN))
         | (KnightAttacks[s] & pieces(KNIGHT))
         | (bishop_attacks(s, occ) & pieces(BISHOP, QUEEN))
         | (rook_attacks(s, occ) & pieces(ROOK, QUEEN))
         | (KingAttacks[s] & pieces(KING));
}

void Position::make_move(Move m) {
    const Color  us = sideToMove, them = ~us;
    const Square from = m.from(), to = m.to();
    const Piece  pc       = piece_on(from);
    const Piece  captured = m.type() == EN_PASSANT ? make_piece(them, PAWN) : piece_on(to);

    StateInfo next = st();
    next.captured  = captured;
    ++next.halfmove;
    Key k = next.key ^ Zobrist::side;

    if (next.ep != NO_SQUARE) {
        k ^= Zobrist::enpassant[file_of(next.ep)];
        next.ep = NO_SQUARE;
    }

    if (m.type() == CASTLING) {
        const auto [rfrom, rto] = castling_rook_squares(from, to);
        const Piece rook        = make_piece(us, ROOK);
        move_piece(from, to);
        move_piece(rfrom, rto);
        k ^= Zobrist::psq[pc][from] ^ Zobrist::psq[pc][to]
           ^ Zobrist::psq[rook][rfrom] ^ Zobrist::psq[rook][rto];
    } else {
        if (captured != NO_PIECE) {
            const Square capsq = m.type() == EN_PASSANT ? to - pawn_push(us) : to;
            remove_piece(capsq);
            k ^= Zobrist::psq[captured][capsq];
            next.halfmove = 0;
        }

        move_piece(from, to);
        k ^= Zobrist::psq[pc][from] ^ Zobrist::psq[pc][to];

        if (type_of(pc) == PAWN) {
            next.halfmove = 0;
            if (m.type() == PROMOTION) {
                const Piece promo = make_piece(us, m.promotion_type());
                remove_piece(to);
                add_piece(promo, to);
                k ^= Zobrist::psq[pc][to] ^ Zobrist::psq[promo][to];
            } else if ((int(from) ^ int(to)) == 16) {  // double push
                const Square epsq = Square((int(from) + int(to)) / 2);
                if (pawn_attacks(us, epsq) & pieces(them, PAWN)) {
                    next.ep = epsq;
                    k ^= Zobrist::enpassant[file_of(epsq)];
                }
            }
        }
    }

    const int rights = next.castling & CastlingMask[from] & CastlingMask[to];
    if (rights != next.castling) {
        k ^= Zobrist::castling[next.castling] ^ Zobrist::castling[rights];
        next.castling = rights;
    }

    next.key = k;
    history.push_back(next);
    sideToMove = them;
    ++gamePly;

    assert(is_consistent());
}

void Position::unmake_move(Move m) {
    sideToMove = ~sideToMove;
    --gamePly;

    const Color  us = sideToMove;
    const Square from = m.from(), to = m.to();
    const Piece  captured = st().captured;

    if (m.type() == CASTLING) {
        const auto [rfrom, rto] = castling_rook_squares(from, to);
        move_piece(to, from);
        move_piece(rto, rfrom);
    } else {
        if (m.type() == PROMOTION) {
            remove_piece(to);
            add_piece(make_piece(us, PAWN), to);
        }
        move_piece(to, from);
        if (captured != NO_PIECE)
            add_piece(captured, m.type() == EN_PASSANT ? to - pawn_push(us) : to);
    }

    history.pop_back();

    assert(is_consistent());
}

void Position::make_null_move() {
    assert(!in_check());
    StateInfo next = st();
    next.captured  = NO_PIECE;
    ++next.halfmove;
    next.key ^= Zobrist::side;
    if (next.ep != NO_SQUARE) {
        next.key ^= Zobrist::enpassant[file_of(next.ep)];
        next.ep = NO_SQUARE;
    }
    history.push_back(next);
    sideToMove = ~sideToMove;
    ++gamePly;
}

void Position::unmake_null_move() {
    history.pop_back();
    sideToMove = ~sideToMove;
    --gamePly;
}

bool Position::is_draw() const {
    if (st().halfmove >= 100)
        return true;

    // Same key, same side to move, within the stretch since the last capture or pawn
    // move (nothing earlier can repeat).
    const int n = int(history.size());
    for (int i = n - 3; i >= 0 && i >= n - 1 - st().halfmove; i -= 2)
        if (history[i].key == st().key)
            return true;

    // Bare kings, or a single knight or bishop.
    if (pieces(PAWN) | pieces(ROOK) | pieces(QUEEN))
        return false;
    return popcount(pieces(KNIGHT) | pieces(BISHOP)) <= 1;
}

Key Position::compute_key() const {
    Key k = 0;
    for (Square s = A1; s <= H8; ++s)
        if (board[s] != NO_PIECE)
            k ^= Zobrist::psq[board[s]][s];
    if (sideToMove == BLACK)
        k ^= Zobrist::side;
    if (ep_square() != NO_SQUARE)
        k ^= Zobrist::enpassant[file_of(ep_square())];
    return k ^ Zobrist::castling[castling_rights()];
}

bool Position::is_consistent() const {
    Bitboard byType[PIECE_TYPE_NB] = {}, byColor[COLOR_NB] = {};
    Score    psq;
    int      phase = 0;
    for (Square s = A1; s <= H8; ++s)
        if (board[s] != NO_PIECE) {
            byType[type_of(board[s])] |= square_bb(s);
            byColor[color_of(board[s])] |= square_bb(s);
            psq += Eval::PieceSquare[board[s]][s];
            phase += Eval::PhaseWeight[type_of(board[s])];
        }
    if (psq != psqScore || phase != gamePhase)
        return false;

    for (int pt = PAWN; pt <= KING; ++pt)
        if (byType[pt] != byTypeBB[pt])
            return false;
    for (int c = WHITE; c <= BLACK; ++c)
        if (byColor[c] != byColorBB[c])
            return false;

    return popcount(pieces(WHITE, KING)) == 1 && popcount(pieces(BLACK, KING)) == 1
        && key() == compute_key();
}
