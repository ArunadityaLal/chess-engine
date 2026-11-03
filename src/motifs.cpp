#include "motifs.h"

#include "movegen.h"

namespace {

constexpr int Value[PIECE_TYPE_NB] = {1, 3, 3, 5, 9, 100};

std::string name_on(const Position& pos, Square s) {
    const PieceType pt = type_of(pos.piece_on(s));
    return (pt == PAWN ? std::string("pawn on ") : std::string(1, "PNBRQK"[pt])) + to_string(s);
}

Bitboard attacks_of(const Position& pos, Square s, Bitboard occ) {
    const Piece pc = pos.piece_on(s);
    return type_of(pc) == PAWN ? pawn_attacks(color_of(pc), s) : attacks_bb(type_of(pc), s, occ);
}

bool defended(const Position& pos, Square s) {
    const Color owner = color_of(pos.piece_on(s));
    return pos.attackers_to(s, pos.pieces()) & pos.pieces(owner);
}

// The piece of `them` standing behind `front` as seen from the slider on `s`, if any.
Square behind(const Position& pos, Square s, Square front) {
    const PieceType pt = type_of(pos.piece_on(s));
    if (pt != BISHOP && pt != ROOK && pt != QUEEN)
        return NO_SQUARE;
    const Bitboard occ      = pos.pieces();
    const Bitboard revealed = attacks_bb(pt, s, occ ^ square_bb(front)) & ~attacks_bb(pt, s, occ);
    const Bitboard next     = revealed & occ;
    return next ? lsb(next) : NO_SQUARE;
}

// Targets worth attacking for a piece of value v: the king, anything more valuable, or
// anything undefended.
bool target(const Position& pos, Square s, int attackerValue) {
    const PieceType pt = type_of(pos.piece_on(s));
    return pt == KING || Value[pt] > attackerValue || !defended(pos, s);
}

}  // namespace

std::vector<Motifs::Motif> Motifs::find(const Position& before, Move m) {
    std::vector<Motif> out;
    Position           pos   = before;
    const Color        us    = before.side_to_move();
    const Color        them  = ~us;
    const Square       to    = m.to();
    const Piece        taken = m.type() == EN_PASSANT ? make_piece(them, PAWN) : before.piece_on(to);

    if (taken != NO_PIECE && m.type() != CASTLING && !defended(before, to))
        out.push_back({"hanging piece", "takes the undefended " + name_on(before, to)});
    if (taken == NO_PIECE && m.type() != CASTLING && !before.see_ge(m, 0))
        out.push_back({"sacrifice", "offers the " + name_on(before, m.from())});
    else if (taken != NO_PIECE && !before.see_ge(m, 0))
        out.push_back({"sacrifice", "gives up material on " + to_string(to)});

    pos.make_move(m);
    const Square landed = m.type() == CASTLING ? (to > m.from() ? m.from() + 1 : m.from() - 1) : to;
    const Bitboard occ  = pos.pieces();

    if (m.type() == PROMOTION)
        out.push_back({"promotion", "promotes to a " + std::string(m.promotion_type() == QUEEN ? "queen"
                                     : m.promotion_type() == ROOK ? "rook" : m.promotion_type() == BISHOP
                                     ? "bishop" : "knight")});

    MoveList replies;
    generate_legal(pos, replies);
    const bool check = pos.in_check();
    if (check && !replies.size())
        out.push_back({"checkmate", ""});
    else if (check) {
        // Check by a piece other than the one that moved: a discovered (or double) check.
        const Bitboard checkers = pos.attackers_to(pos.king_square(them), occ) & pos.pieces(us);
        if (checkers & ~square_bb(landed))
            out.push_back({checkers & square_bb(landed) ? "double check" : "discovered check",
                           "by " + name_on(pos, lsb(checkers & ~square_bb(landed)))});
        else
            out.push_back({"check", ""});
    }

    // Fork: the moved piece now attacks two or more worthwhile targets.
    const PieceType movedType = type_of(pos.piece_on(landed));
    std::vector<Square> forked;
    for (Bitboard b = attacks_of(pos, landed, occ) & pos.pieces(them); b;) {
        const Square s = pop_lsb(b);
        if (target(pos, s, Value[movedType]))
            forked.push_back(s);
    }
    if (forked.size() >= 2) {
        std::string d = name_on(pos, landed) + " attacks";
        for (size_t i = 0; i < forked.size(); ++i)
            d += (i ? i + 1 == forked.size() ? " and " : ", " : " ") + name_on(pos, forked[i]);
        out.push_back({"fork", d});
    }

    // Pin and skewer by the moved piece: an enemy piece on its line with a more valuable
    // one behind (pin: the front piece can't move away safely) or a less valuable one
    // behind a more valuable front piece (skewer: the front one moves, the back one falls).
    for (Bitboard b = attacks_of(pos, landed, occ) & pos.pieces(them); b;) {
        const Square front = pop_lsb(b);
        const Square back  = behind(pos, landed, front);
        if (back == NO_SQUARE || color_of(pos.piece_on(back)) != them)
            continue;
        const int vf = Value[type_of(pos.piece_on(front))], vb = Value[type_of(pos.piece_on(back))];
        if (vb > vf && type_of(pos.piece_on(front)) != KING)
            out.push_back({"pin", name_on(pos, landed) + " pins " + name_on(pos, front) + " to "
                                      + name_on(pos, back)});
        else if (vf > vb && (vf >= Value[ROOK] || type_of(pos.piece_on(front)) == KING))
            out.push_back({"skewer", name_on(pos, landed) + " skewers " + name_on(pos, front)
                                         + " and " + name_on(pos, back)});
    }

    // Discovered attack: another of our pieces now attacks a target it didn't before,
    // because the moved piece got out of its way.
    for (Bitboard ours = pos.pieces(us) & ~square_bb(landed) & ~pos.pieces(PAWN, KING); ours;) {
        const Square   s     = pop_lsb(ours);
        const Bitboard now   = attacks_of(pos, s, occ) & pos.pieces(them) & ~pos.pieces(KING);
        const Bitboard was   = attacks_of(before, s, before.pieces()) & before.pieces(them);
        for (Bitboard fresh = now & ~was; fresh;) {
            const Square t = pop_lsb(fresh);
            if (target(pos, t, Value[type_of(pos.piece_on(s))])) {
                out.push_back({"discovered attack", name_on(pos, s) + " now attacks " + name_on(pos, t)});
                break;
            }
        }
    }
    return out;
}
