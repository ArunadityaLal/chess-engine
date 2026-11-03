#pragma once

#include <string>
#include <vector>

#include "bitboard.h"
#include "eval.h"
#include "nnue.h"

constexpr const char* StartFEN = "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1";

// Everything unmake_move can't cheaply recompute. One entry per ply.
struct StateInfo {
    Key    key;
    int    castling;  // WHITE_OO | WHITE_OOO | BLACK_OO | BLACK_OOO
    Square ep;        // only set when a pawn can actually capture there
    int    halfmove;  // plies since the last capture or pawn move (50-move rule)
    Piece  captured;  // piece captured by the move that led to this state
};

class Position {
  public:
    static void init();  // Zobrist keys; call once after Bitboards::init()

    Position();  // starting position
    bool        set(const std::string& fen);  // false on a malformed FEN, position unchanged
    std::string fen() const;
    std::string pretty() const;

    Bitboard pieces() const { return byColorBB[WHITE] | byColorBB[BLACK]; }
    Bitboard pieces(Color c) const { return byColorBB[c]; }
    Bitboard pieces(PieceType pt) const { return byTypeBB[pt]; }
    Bitboard pieces(PieceType a, PieceType b) const { return byTypeBB[a] | byTypeBB[b]; }
    Bitboard pieces(Color c, PieceType pt) const { return byColorBB[c] & byTypeBB[pt]; }
    Piece    piece_on(Square s) const { return board[s]; }
    Square   king_square(Color c) const { return lsb(pieces(c, KING)); }

    Color  side_to_move() const { return sideToMove; }
    int    castling_rights() const { return st().castling; }
    Square ep_square() const { return st().ep; }
    int    halfmove_clock() const { return st().halfmove; }
    int    game_ply() const { return gamePly; }
    Key    key() const { return st().key; }

    // Running sums kept up to date by the piece hooks; Eval::evaluate reads them.
    Score psq_score() const { return psqScore; }
    int   phase() const { return gamePhase; }
    const NNUE::Accumulator& accumulator() const { return acc; }  // when NNUE::enabled
    void  refresh_eval();  // recompute both after Eval::use() switches evaluations

    // 50-move rule, repetition (one repeat counts, as is usual inside a search), or
    // material that can't possibly mate.
    bool is_draw() const;

    Bitboard attackers_to(Square s, Bitboard occ) const;
    bool     attacked_by(Color c, Square s) const { return attackers_to(s, pieces()) & pieces(c); }
    bool     in_check() const { return attacked_by(~sideToMove, king_square(sideToMove)); }
    // Call right after make_move: true if the move left the mover's own king attacked.
    bool     mover_in_check() const { return attacked_by(sideToMove, king_square(~sideToMove)); }

    void make_move(Move m);
    void unmake_move(Move m);

    // Static exchange evaluation: if both sides keep capturing on the move's target
    // square, always with their least valuable piece and stopping when that would lose,
    // does the mover come out at least `threshold` centipawns ahead? Uses fixed values
    // P 100, N 300, B 300, R 500, Q 900 (not the eval's), and ignores pins and checks.
    bool see_ge(Move m, int threshold) const;

    // Pass the turn. Not a legal chess move: used to ask "what is the opponent
    // threatening?" and for null-move pruning. Never call while in check. Resets the
    // 50-move counter, so no repetition is detected across the pass.
    void make_null_move();
    void unmake_null_move();

    Key  compute_key() const;    // from scratch, to check the incremental key
    bool is_consistent() const;  // debug: bitboards, board[], key and eval sums all agree

  private:
    bool parse(const std::string& fen);
    void clear();

    // The only functions that modify board[] and the bitboards. They also keep the
    // piece-square sum, game phase and NNUE accumulator current.
    void add_piece(Piece p, Square s);
    void remove_piece(Square s);
    void move_piece(Square from, Square to);

    StateInfo&       st() { return history.back(); }
    const StateInfo& st() const { return history.back(); }

    Piece                  board[SQUARE_NB];
    Bitboard               byTypeBB[PIECE_TYPE_NB];
    Bitboard               byColorBB[COLOR_NB];
    Color                  sideToMove;
    int                    gamePly;
    Score                  psqScore;
    int                    gamePhase;
    NNUE::Accumulator      acc;
    std::vector<StateInfo> history;
};
