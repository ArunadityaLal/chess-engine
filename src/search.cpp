#include "search.h"

#include <algorithm>
#include <cstdlib>

#include "eval.h"
#include "movegen.h"
#include "uci.h"

namespace {

// Captures and queen promotions: the moves quiescence search looks at.
bool is_tactical(const Position& pos, Move m) {
    return pos.piece_on(m.to()) != NO_PIECE || m.type() == EN_PASSANT
        || (m.type() == PROMOTION && m.promotion_type() == QUEEN);
}

// Move ordering. Alpha-beta prunes most when the best move is tried first:
// 1. the move from the previous iteration's best line,
// 2. captures, most valuable victim first, then least valuable attacker (MVV-LVA),
// 3. queen promotions, 4. everything else.
void score_moves(const Position& pos, const MoveList& list, int* scores, Move hint) {
    for (int i = 0; i < list.size(); ++i) {
        const Move m = list.moves[i];
        if (m == hint)
            scores[i] = 1'000'000;
        else if (pos.piece_on(m.to()) != NO_PIECE || m.type() == EN_PASSANT) {
            const PieceType victim = m.type() == EN_PASSANT ? PAWN : type_of(pos.piece_on(m.to()));
            scores[i] = 100'000 + 10 * victim - type_of(pos.piece_on(m.from()));
        } else if (m.type() == PROMOTION && m.promotion_type() == QUEEN)
            scores[i] = 90'000;
        else
            scores[i] = 0;
    }
}

// Selection sort, one step: bring the best remaining move to index i. Cheaper than a
// full sort because a cutoff usually comes after the first few moves.
Move pick_next(MoveList& list, int* scores, int i) {
    int best = i;
    for (int j = i + 1; j < list.size(); ++j)
        if (scores[j] > scores[best])
            best = j;
    std::swap(list.moves[i], list.moves[best]);
    std::swap(scores[i], scores[best]);
    return list.moves[i];
}

std::string uci_score(int s) {
    if (s >= MATE_BOUND)
        return "mate " + std::to_string((MATE - s + 1) / 2);
    if (s <= -MATE_BOUND)
        return "mate " + std::to_string(-(MATE + s) / 2);
    return "cp " + std::to_string(s);
}

std::string human_score(int s, Color us) {
    const std::string side = us == WHITE ? "White" : "Black";
    const std::string them = us == WHITE ? "Black" : "White";
    if (s >= MATE_BOUND)
        return side + " mates in " + std::to_string((MATE - s + 1) / 2);
    if (s <= -MATE_BOUND)
        return them + " mates in " + std::to_string((MATE + s) / 2);
    const int white = us == WHITE ? s : -s;
    if (white == 0)
        return "level";
    return (white > 0 ? "White " : "Black ") + Eval::format_score(std::abs(white));
}

}  // namespace

Searcher::Searcher() : rng(std::random_device{}()) {}

Searcher::~Searcher() {
    stop();
    wait();
}

void Searcher::start(const Position& pos, const SearchLimits& l, const EngineOptions& o) {
    stop();
    wait();
    rootPos       = pos;
    limits        = l;
    options       = o;
    stopRequested = false;
    thread        = std::thread(&Searcher::run, this);
}

void Searcher::stop() { stopRequested = true; }

void Searcher::wait() {
    if (thread.joinable())
        thread.join();
}

void Searcher::run() {
    const SearchResult result = think(rootPos, true);

    // UCI: after "go infinite", bestmove may only be sent once "stop" arrives.
    while (limits.infinite && !stopRequested)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));

    if (result.lines.empty()) {
        UCI::send(std::string("info depth 0 score ") + (rootPos.in_check() ? "mate 0" : "cp 0"));
        UCI::send("bestmove 0000");
        return;
    }
    const RootMove& best = result.lines[0];
    std::string     out  = "bestmove " + to_uci(best.move);
    if (best.pv.size() > 1)
        out += " ponder " + to_uci(best.pv[1]);
    UCI::send(out);
}

SearchResult Searcher::search(const Position& pos, const SearchLimits& l,
                              const EngineOptions& o, bool print) {
    limits        = l;
    options       = o;
    stopRequested = false;
    return think(pos, print);
}

// Iterative deepening: search depth 1, 2, 3, ... until a limit is hit. Each iteration
// orders moves for the next (best line first), and there is always a finished answer to
// fall back on when time runs out mid-iteration.
SearchResult Searcher::think(const Position& pos, bool print) {
    startTime = std::chrono::steady_clock::now();
    nodes     = 0;
    aborted   = false;
    std::fill(std::begin(pvHint), std::end(pvHint), Move::none());

    Position root = pos;
    set_time_limits(root.side_to_move());

    MoveList legal;
    generate_legal(root, legal);
    int scores[MAX_MOVES];
    score_moves(root, legal, scores, Move::none());
    rootMoves.clear();
    for (int i = 0; i < legal.size(); ++i)
        rootMoves.push_back({pick_next(legal, scores, i), -INF, {}});

    SearchResult result;
    if (rootMoves.empty())
        return result;

    if (options.randomMover) {
        RootMove rm = rootMoves[rng() % rootMoves.size()];
        rm.pv       = {rm.move};
        result.lines.push_back(rm);
        return result;
    }

    const int maxDepth = limits.depth ? std::min(limits.depth, MAX_PLY - 1) : MAX_PLY - 1;
    const int multiPV  = std::min<int>(options.multiPV, int(rootMoves.size()));
    Move      prevBest  = Move::none();
    int       prevScore = 0;

    for (int depth = 1; depth <= maxDepth; ++depth) {
        selDepth = 0;
        for (int pvIdx = 0; pvIdx < multiPV && !aborted; ++pvIdx)
            search_root(root, depth, pvIdx);
        if (aborted)
            break;  // keep the last completed iteration

        result.lines.assign(rootMoves.begin(), rootMoves.begin() + multiPV);
        result.depth = depth;
        const RootMove& best = rootMoves[0];
        for (size_t i = 0; i < best.pv.size() && i < MAX_PLY; ++i)
            pvHint[i] = best.pv[i];

        if (print) {
            report(depth, multiPV);
            if (options.thinkAloud)
                think_aloud(root, depth, prevBest, prevScore);
        }
        prevBest  = best.move;
        prevScore = best.score;

        // On the clock: a proven mate won't get any better, and starting an iteration
        // past the soft limit would likely be cut off by the hard limit anyway.
        if (softLimit) {
            if (std::abs(best.score) >= MATE_BOUND && depth > MATE - std::abs(best.score))
                break;
            if (elapsed_ms() >= softLimit)
                break;
        }
    }

    if (result.lines.empty())  // aborted inside depth 1: best guess by move ordering
        result.lines = {RootMove{rootMoves[0].move, 0, {rootMoves[0].move}}};
    result.nodes = nodes;

    if (print && options.thinkAloud) {
        const RootMove& best = result.lines[0];
        UCI::send("info string [think] decision: " + to_san(root, best.move) + " after "
                  + std::to_string(nodes) + " positions in " + std::to_string(elapsed_ms())
                  + " ms, depth " + std::to_string(result.depth) + ". Expected line: "
                  + line_to_san(root, best.pv));
    }
    return result;
}

// Root moves [pvIdx, end) are searched with a full window; the best of them lands at
// pvIdx with an exact score. Calling this for pvIdx = 0, 1, 2 gives MultiPV lines.
void Searcher::search_root(Position& pos, int depth, int pvIdx) {
    int alpha = -INF;
    for (size_t i = pvIdx; i < rootMoves.size(); ++i) {
        RootMove& rm = rootMoves[i];
        pos.make_move(rm.move);
        ++nodes;
        const int score = -negamax(pos, depth - 1, 1, -INF, -alpha);
        pos.unmake_move(rm.move);
        if (aborted)
            return;

        if (score > alpha) {
            alpha    = score;
            rm.score = score;
            rm.pv.assign(1, rm.move);
            rm.pv.insert(rm.pv.end(), &pvTable[1][1], &pvTable[1][pvLength[1]]);
        } else
            rm.score = -INF;  // only an upper bound; sorts behind, keeping the old order
    }
    std::stable_sort(rootMoves.begin() + pvIdx, rootMoves.end(),
                     [](const RootMove& a, const RootMove& b) { return a.score > b.score; });
}

// Fail-soft alpha-beta in negamax form: every score is from the side to move's view, so
// a child's score is negated. [alpha, beta] is the window of scores that still matter:
// a move scoring >= beta means the opponent will avoid this position anyway (cutoff).
int Searcher::negamax(Position& pos, int depth, int ply, int alpha, int beta) {
    pvLength[ply] = ply;
    if (should_stop())
        return 0;
    if (pos.is_draw())
        return 0;
    if (depth <= 0)
        return options.quiescence ? qsearch(pos, ply, alpha, beta) : Eval::evaluate(pos);
    if (ply >= MAX_PLY - 1)
        return Eval::evaluate(pos);

    const bool inCheck = pos.in_check();
    MoveList   list;
    generate_pseudo_legal(pos, list);
    int scores[MAX_MOVES];
    score_moves(pos, list, scores, pvHint[ply]);

    int best = -INF, legal = 0;
    for (int i = 0; i < list.size(); ++i) {
        const Move m = pick_next(list, scores, i);
        pos.make_move(m);
        if (pos.mover_in_check()) {
            pos.unmake_move(m);
            continue;
        }
        ++legal;
        ++nodes;
        const int score = -negamax(pos, depth - 1, ply + 1, -beta, -alpha);
        pos.unmake_move(m);
        if (aborted)
            return 0;

        if (score > best) {
            best = score;
            if (score > alpha) {
                alpha = score;
                update_pv(ply, m);
                if (alpha >= beta)
                    break;
            }
        }
    }

    if (!legal)
        return inCheck ? -MATE + ply : 0;  // checkmate (sooner is worse) or stalemate
    return best;
}

// Quiescence search: at the end of the main search, keep playing captures until the
// position is quiet, so the eval never sees a half-finished exchange (horizon effect).
int Searcher::qsearch(Position& pos, int ply, int alpha, int beta) {
    pvLength[ply] = ply;
    if (should_stop())
        return 0;
    selDepth = std::max(selDepth, ply);
    if (ply >= MAX_PLY - 1)
        return Eval::evaluate(pos);

    // Stand pat: the side to move isn't forced to capture, so the static eval is a lower
    // bound. In check there is no such option: every evasion must be searched.
    const bool inCheck = pos.in_check();
    int        best    = -INF;
    if (!inCheck) {
        best = Eval::evaluate(pos);
        if (best >= beta)
            return best;
        alpha = std::max(alpha, best);
    }

    MoveList list;
    generate_pseudo_legal(pos, list);
    int scores[MAX_MOVES];
    score_moves(pos, list, scores, Move::none());

    int legal = 0;
    for (int i = 0; i < list.size(); ++i) {
        const Move m = pick_next(list, scores, i);
        if (!inCheck && !is_tactical(pos, m))
            continue;
        pos.make_move(m);
        if (pos.mover_in_check()) {
            pos.unmake_move(m);
            continue;
        }
        ++legal;
        ++nodes;
        const int score = -qsearch(pos, ply + 1, -beta, -alpha);
        pos.unmake_move(m);
        if (aborted)
            return 0;

        if (score > best) {
            best = score;
            if (score > alpha) {
                alpha = score;
                update_pv(ply, m);
                if (alpha >= beta)
                    break;
            }
        }
    }

    if (inCheck && !legal)
        return -MATE + ply;
    return best;
}

int Searcher::minimax(const Position& pos, int depth, const EngineOptions& o) {
    Position p    = pos;
    limits        = {};
    options       = o;
    softLimit     = hardLimit = 0;
    stopRequested = false;
    aborted       = false;
    nodes         = 0;
    return minimax(p, depth, 0);
}

int Searcher::quiescence(const Position& pos, bool bruteForce) {
    Position p    = pos;
    limits        = {};
    softLimit     = hardLimit = 0;
    stopRequested = false;
    aborted       = false;
    nodes         = 0;
    return bruteForce ? minimax_q(p, 0) : qsearch(p, 0, -INF, INF);
}

// Same rules as negamax (draws, mates), but no window and no cutoffs.
int Searcher::minimax(Position& pos, int depth, int ply) {
    if (ply > 0 && pos.is_draw())
        return 0;
    if (depth <= 0)
        return options.quiescence ? qsearch(pos, ply, -INF, INF) : Eval::evaluate(pos);
    if (ply >= MAX_PLY - 1)
        return Eval::evaluate(pos);

    MoveList list;
    generate_pseudo_legal(pos, list);
    int best = -INF, legal = 0;
    for (Move m : list) {
        pos.make_move(m);
        if (!pos.mover_in_check()) {
            ++legal;
            best = std::max(best, -minimax(pos, depth - 1, ply + 1));
        }
        pos.unmake_move(m);
    }
    if (!legal)
        return pos.in_check() ? -MATE + ply : 0;
    return best;
}

// Same rules as qsearch (stand pat, captures only unless in check), no cutoffs.
int Searcher::minimax_q(Position& pos, int ply) {
    if (ply >= MAX_PLY - 1)
        return Eval::evaluate(pos);

    const bool inCheck = pos.in_check();
    int        best    = inCheck ? -INF : Eval::evaluate(pos);
    int        legal   = 0;

    MoveList list;
    generate_pseudo_legal(pos, list);
    for (Move m : list) {
        if (!inCheck && !is_tactical(pos, m))
            continue;
        pos.make_move(m);
        if (!pos.mover_in_check()) {
            ++legal;
            best = std::max(best, -minimax_q(pos, ply + 1));
        }
        pos.unmake_move(m);
    }
    if (inCheck && !legal)
        return -MATE + ply;
    return best;
}

// Soft limit: don't start a new iteration after this. Hard limit: abort mid-search.
void Searcher::set_time_limits(Color us) {
    softLimit = hardLimit = 0;
    if (limits.movetime)
        softLimit = hardLimit = std::max<int64_t>(1, limits.movetime - options.moveOverhead);
    else if (limits.time[us]) {
        const int64_t time   = limits.time[us];
        const int64_t usable = std::max<int64_t>(1, time - options.moveOverhead);
        // Without movestogo, plan as if 30 more moves are needed (sudden death).
        const int movesLeft = limits.movestogo ? std::min(limits.movestogo, 40) : 30;
        hardLimit = std::max<int64_t>(1, usable * 3 / 4);
        softLimit = std::min(hardLimit, time / movesLeft + limits.inc[us] * 3 / 4);
        hardLimit = std::min(hardLimit, softLimit * 4);
    }
}

bool Searcher::should_stop() {
    if (aborted)
        return true;
    // Reading the clock costs more than a node, so only every 1024 nodes.
    if ((nodes & 1023) == 0 && (stopRequested || (hardLimit && elapsed_ms() >= hardLimit)))
        aborted = true;
    if (limits.nodes && nodes >= limits.nodes)
        aborted = true;
    return aborted;
}

int64_t Searcher::elapsed_ms() const {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now() - startTime).count();
}

// Triangular PV table: row ply holds the best line found from that ply on.
void Searcher::update_pv(int ply, Move m) {
    pvTable[ply][ply] = m;
    for (int i = ply + 1; i < pvLength[ply + 1]; ++i)
        pvTable[ply][i] = pvTable[ply + 1][i];
    pvLength[ply] = pvLength[ply + 1];
}

void Searcher::report(int depth, int lines) const {
    const int64_t ms = elapsed_ms();
    for (int i = 0; i < lines; ++i) {
        const RootMove& rm = rootMoves[i];
        std::string     pv;
        for (Move m : rm.pv)
            pv += ' ' + to_uci(m);
        UCI::send("info depth " + std::to_string(depth) + " seldepth " + std::to_string(selDepth)
                  + " multipv " + std::to_string(i + 1) + " score " + uci_score(rm.score)
                  + " nodes " + std::to_string(nodes) + " nps "
                  + std::to_string(nodes * 1000 / (ms + 1)) + " time " + std::to_string(ms)
                  + " pv" + pv);
    }
}

// Plain-language commentary on each iteration: what it now thinks is best, and when a
// deeper look changed its mind.
void Searcher::think_aloud(const Position& pos, int depth, Move prevBest, int prevScore) const {
    Position        p    = pos;
    const RootMove& best = rootMoves[0];
    const Color     us   = pos.side_to_move();
    std::string     msg  = "info string [think] depth " + std::to_string(depth) + ": ";

    if (prevBest.is_ok() && prevBest != best.move)
        msg += "changed my mind. " + to_san(p, prevBest) + " looked best one move shallower ("
             + human_score(prevScore, us) + "), but deeper search prefers "
             + to_san(p, best.move) + " (" + human_score(best.score, us) + ").";
    else
        msg += to_san(p, best.move) + " is best (" + human_score(best.score, us) + ").";

    msg += " Line: " + line_to_san(p, best.pv);

    if (options.multiPV > 1 && rootMoves.size() > 1 && rootMoves[1].score > -INF)
        msg += ". Runner-up: " + to_san(p, rootMoves[1].move) + " ("
             + human_score(rootMoves[1].score, us) + ")";
    UCI::send(msg);
}
