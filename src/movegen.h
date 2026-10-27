#pragma once

#include <string>
#include <vector>

#include "position.h"

constexpr int MAX_MOVES = 256;  // the most legal moves in any chess position is 218

struct MoveList {
    Move moves[MAX_MOVES];
    int  count = 0;

    void add(Move m) { moves[count++] = m; }
    int  size() const { return count; }

    Move*       begin() { return moves; }
    Move*       end() { return moves + count; }
    const Move* begin() const { return moves; }
    const Move* end() const { return moves + count; }
};

// All moves that follow piece movement rules; some may leave the king in check.
// Search and perft filter those with make_move + mover_in_check.
void generate_pseudo_legal(const Position& pos, MoveList& list);

// Only legal moves. Slower (make/unmake per move); for the root and UCI input.
void generate_legal(Position& pos, MoveList& list);

// The legal move matching a UCI string like "e2e4" or "e7e8q", or Move::none().
Move parse_uci_move(Position& pos, const std::string& str);

// Standard algebraic notation for a legal move: "Nbd7", "exd5", "O-O", "e8=Q+", "Qxf7#".
std::string to_san(Position& pos, Move m);

// A line of moves in SAN with move numbers, e.g. "12. Nf5 exf5 13. Qxd6" or "12... exf5".
std::string line_to_san(Position pos, const std::vector<Move>& line);
