#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <random>
#include <thread>
#include <vector>

#include "position.h"

constexpr int MAX_PLY    = 128;
constexpr int INF        = 32001;
constexpr int MATE       = 32000;            // score for "mate right now"; minus plies to it
constexpr int MATE_BOUND = MATE - MAX_PLY;   // anything beyond this is a forced mate

// What "go" asked for. Zero means "not given".
struct SearchLimits {
    int64_t  time[COLOR_NB] = {};  // ms left on each clock
    int64_t  inc[COLOR_NB]  = {};  // ms added per move
    int      movestogo      = 0;
    int      depth          = 0;
    int64_t  movetime       = 0;
    uint64_t nodes          = 0;
    bool     infinite       = false;
    int      perft          = 0;  // "go perft N": debugging, not a search
};

enum class StyleKind { Objective, Creative, Practical };  // see style.h

// UCI options that affect searching.
struct EngineOptions {
    int  multiPV      = 1;      // how many best lines to report
    int  threads      = 1;      // search threads (Lazy SMP)
    int  moveOverhead = 30;     // ms kept in reserve per move for GUI and pipe lag
    bool thinkAloud   = false;  // explain each iteration in plain language
    bool randomMover  = false;  // play random legal moves (a test opponent)
    int  contempt     = 0;      // cp: a draw is worth -contempt to the side that searches
    StyleKind style   = StyleKind::Objective;
    int  variety      = 0;      // cp: in the opening, pick randomly among moves this close
    bool quiescence   = true;   // tests only: false evaluates leaves statically
    bool exact        = false;  // tests only: plain fixed-depth alpha-beta (no table cutoffs,
                                // pruning, reductions or extensions), so scores equal minimax
};

struct RootMove {
    Move              move;
    int               score = -INF;  // exact for the lines that were searched as PVs
    std::vector<Move> pv;            // starts with move
};

struct SearchResult {
    std::vector<RootMove> lines;  // best first; empty if there are no legal moves
    int                   depth = 0;
    uint64_t              nodes = 0;
};

// Runs the search on its own thread so the UCI loop stays responsive to isready and stop.
class Searcher {
  public:
    Searcher();
    ~Searcher();

    // For UCI: returns immediately; prints info lines and then bestmove.
    void start(const Position& pos, const SearchLimits& limits, const EngineOptions& options);
    void stop();  // ask the running search to finish; it still prints bestmove
    void wait();  // block until the search thread is done

    // Forget move-ordering knowledge from earlier searches (new game). Only call when idle.
    void clear();

    // Synchronous, for tests, bench and analysis commands. Only call when idle.
    SearchResult search(const Position& pos, const SearchLimits& limits,
                        const EngineOptions& options, bool print);

    // Correctness oracles. minimax() searches the main tree without any pruning; its
    // leaves use quiescence with a full window (exact), or the static eval. Alpha-beta
    // with the same options must return exactly the same score at the same depth.
    int minimax(const Position& pos, int depth, const EngineOptions& options);
    // Quiescence alone: with bruteForce, every capture sequence with no pruning (can
    // explode in tactical positions); otherwise alpha-beta. Both must agree.
    int quiescence(const Position& pos, bool bruteForce);

  private:
    void         run();
    // Lazy SMP helper: searches like the main thread, without printing or time control,
    // until the main thread stops it. Shares only the transposition table.
    void         helper_search(const Position& pos, const SearchLimits& limits,
                               const EngineOptions& options);
    uint64_t     total_nodes() const;
    SearchResult think(const Position& pos, bool print);
    int          search_root(Position& pos, int depth, int pvIdx, int alpha, int beta);
    int          negamax(Position& pos, int depth, int ply, int alpha, int beta,
                         bool canNull = true);
    int          qsearch(Position& pos, int ply, int alpha, int beta);
    void         update_quiet_stats(const Position& pos, int ply, int depth, Move best,
                                    const Move* quiets, int quietCount);
    int16_t*     cont_row(int ply);  // continuation history after the move made at ply
    int          minimax(Position& pos, int depth, int ply);
    int          minimax_q(Position& pos, int ply);

    void    set_time_limits(Color us);
    bool    should_stop();
    int64_t elapsed_ms() const;
    void    update_pv(int ply, Move m);
    int     draw_score(int ply) const;
    void    report(int depth, int lines) const;
    void    think_aloud(const Position& pos, int depth, Move prevBest, int prevScore) const;

    std::thread       thread;
    std::atomic<bool> stopRequested{false};

    int                                    helperId = 0;  // 0: the main thread
    std::vector<std::unique_ptr<Searcher>> helpers;
    std::atomic<uint64_t>                  sharedNodes{0};  // a helper's node count, for reports
    std::mt19937_64   rng;

    // Inputs of the current search.
    Position      rootPos;
    SearchLimits  limits;
    EngineOptions options;

    // State of the current search; only touched by the thread that runs it.
    std::vector<RootMove>                 rootMoves;
    std::chrono::steady_clock::time_point startTime;
    int64_t                               softLimit = 0;  // ms: don't start a new iteration
    int64_t                               hardLimit = 0;  // ms: abort mid-iteration
    uint64_t                              nodes     = 0;
    int                                   selDepth  = 0;
    bool                                  aborted   = false;
    Move                                  pvTable[MAX_PLY][MAX_PLY];
    int                                   pvLength[MAX_PLY];
    Move                                  pvHint[MAX_PLY];  // previous iteration's best line
    Move                                  killers[MAX_PLY][2];
    int                                   evalStack[MAX_PLY];  // static eval per ply, -INF in check
    Move                                  excludedMove[MAX_PLY];  // singular search: skip it
    int                                   history[COLOR_NB][SQUARE_NB][SQUARE_NB];
    // pieceTo = piece * 64 + to-square of a move: what continuation history is keyed by.
    static constexpr int PIECE_TO = PIECE_NB * SQUARE_NB;
    int                  movedStack[MAX_PLY];  // pieceTo of the move made at each ply, -1: none
    Move                 counterMoves[PIECE_TO];  // best reply to a pieceTo, by cutoffs
    // contHist[a * PIECE_TO + b]: how good a move b has been one or two plies after a.
    std::vector<int16_t> contHist;
};
