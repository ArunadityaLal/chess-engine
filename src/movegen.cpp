#include "movegen.h"

namespace {

void add_promotions(MoveList& list, Square from, Square to) {
    for (PieceType pt : {QUEEN, ROOK, BISHOP, KNIGHT})
        list.add(Move(from, to, PROMOTION, pt));
}

template<Color Us>
void generate_pawn_moves(const Position& pos, MoveList& list) {
    constexpr Color     Them  = ~Us;
    constexpr Direction Up    = pawn_push(Us);
    constexpr Bitboard  Rank3 = Us == WHITE ? RANK_3_BB : RANK_6_BB;
    constexpr Bitboard  Rank8 = Us == WHITE ? RANK_8_BB : RANK_1_BB;

    const Bitboard pawns   = pos.pieces(Us, PAWN);
    const Bitboard empty   = ~pos.pieces();
    const Bitboard enemies = pos.pieces(Them);

    // Pushes, computed for all pawns at once.
    const Bitboard single = shift<Up>(pawns) & empty;
    const Bitboard dbl    = shift<Up>(single & Rank3) & empty;

    for (Bitboard b = single & ~Rank8; b;) {
        const Square to = pop_lsb(b);
        list.add(Move(to - Up, to));
    }
    for (Bitboard b = single & Rank8; b;) {
        const Square to = pop_lsb(b);
        add_promotions(list, to - Up, to);
    }
    for (Bitboard b = dbl; b;) {
        const Square to = pop_lsb(b);
        list.add(Move(to - 2 * Up, to));
    }

    // Captures, including capture-promotions.
    for (Bitboard b = pawns; b;) {
        const Square from = pop_lsb(b);
        for (Bitboard att = pawn_attacks(Us, from) & enemies; att;) {
            const Square to = pop_lsb(att);
            if (square_bb(to) & Rank8)
                add_promotions(list, from, to);
            else
                list.add(Move(from, to));
        }
    }

    // The pawns that could capture on the e.p. square are those a pawn of the other
    // color standing there would attack.
    if (pos.ep_square() != NO_SQUARE)
        for (Bitboard b = pawn_attacks(Them, pos.ep_square()) & pawns; b;)
            list.add(Move(pop_lsb(b), pos.ep_square(), EN_PASSANT));
}

void generate_castling(const Position& pos, MoveList& list) {
    const Color us        = pos.side_to_move();
    const int   kingSide  = us == WHITE ? WHITE_OO : BLACK_OO;
    const int   queenSide = us == WHITE ? WHITE_OOO : BLACK_OOO;
    const int   rights    = pos.castling_rights() & (kingSide | queenSide);
    if (!rights || pos.in_check())
        return;

    const Color    them = ~us;
    const Square   ksq  = us == WHITE ? E1 : E8;
    const Bitboard occ  = pos.pieces();

    // The king can't pass through an attacked square. Its landing square is checked by
    // the normal legality test, like any other king move.
    if ((rights & kingSide) && !(occ & (square_bb(ksq + 1) | square_bb(ksq + 2)))
        && !pos.attacked_by(them, ksq + 1))
        list.add(Move(ksq, ksq + 2, CASTLING));

    if ((rights & queenSide)
        && !(occ & (square_bb(ksq - 1) | square_bb(ksq - 2) | square_bb(ksq - 3)))
        && !pos.attacked_by(them, ksq - 1))
        list.add(Move(ksq, ksq - 2, CASTLING));
}

}  // namespace

void generate_pseudo_legal(const Position& pos, MoveList& list) {
    const Color us = pos.side_to_move();
    if (us == WHITE)
        generate_pawn_moves<WHITE>(pos, list);
    else
        generate_pawn_moves<BLACK>(pos, list);

    const Bitboard occ     = pos.pieces();
    const Bitboard targets = ~pos.pieces(us);
    for (PieceType pt : {KNIGHT, BISHOP, ROOK, QUEEN, KING})
        for (Bitboard b = pos.pieces(us, pt); b;) {
            const Square from = pop_lsb(b);
            for (Bitboard att = attacks_bb(pt, from, occ) & targets; att;)
                list.add(Move(from, pop_lsb(att)));
        }

    generate_castling(pos, list);
}

void generate_legal(Position& pos, MoveList& list) {
    MoveList pseudo;
    generate_pseudo_legal(pos, pseudo);
    for (Move m : pseudo) {
        pos.make_move(m);
        if (!pos.mover_in_check())
            list.add(m);
        pos.unmake_move(m);
    }
}

Move parse_uci_move(Position& pos, const std::string& str) {
    MoveList list;
    generate_legal(pos, list);
    for (Move m : list)
        if (to_uci(m) == str)
            return m;
    return Move::none();
}

std::string to_san(Position& pos, Move m) {
    const Square    from    = m.from(), to = m.to();
    const PieceType pt      = type_of(pos.piece_on(from));
    const bool      capture = m.type() == EN_PASSANT || pos.piece_on(to) != NO_PIECE;
    std::string     san;

    if (m.type() == CASTLING)
        san = to > from ? "O-O" : "O-O-O";
    else if (pt == PAWN) {
        if (capture)
            san = std::string(1, char('a' + file_of(from))) + 'x';
        san += to_string(to);
        if (m.type() == PROMOTION)
            san += std::string("=") + "NBRQ"[m.promotion_type() - KNIGHT];
    } else {
        san = "PNBRQK"[pt];

        // Another piece of the same type can also reach the target: add the file,
        // the rank, or both, whichever tells them apart.
        MoveList legal;
        generate_legal(pos, legal);
        bool ambiguous = false, sameFile = false, sameRank = false;
        for (Move other : legal)
            if (other != m && other.to() == to && type_of(pos.piece_on(other.from())) == pt) {
                ambiguous = true;
                sameFile |= file_of(other.from()) == file_of(from);
                sameRank |= rank_of(other.from()) == rank_of(from);
            }
        if (ambiguous)
            san += !sameFile ? std::string(1, char('a' + file_of(from)))
                 : !sameRank ? std::string(1, char('1' + rank_of(from)))
                             : to_string(from);

        if (capture)
            san += 'x';
        san += to_string(to);
    }

    pos.make_move(m);
    if (pos.in_check()) {
        MoveList replies;
        generate_legal(pos, replies);
        san += replies.size() ? '+' : '#';
    }
    pos.unmake_move(m);
    return san;
}

std::string line_to_san(Position pos, const std::vector<Move>& line) {
    std::string s;
    for (size_t i = 0; i < line.size(); ++i) {
        const int moveNumber = pos.game_ply() / 2 + 1;
        if (pos.side_to_move() == WHITE)
            s += std::to_string(moveNumber) + ". ";
        else if (i == 0)
            s += std::to_string(moveNumber) + "... ";
        s += to_san(pos, line[i]) + ' ';
        pos.make_move(line[i]);
    }
    if (!s.empty())
        s.pop_back();
    return s;
}
