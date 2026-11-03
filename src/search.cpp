#include "search.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <span>

#include "eval.h"
#include "movegen.h"
#include "style.h"
#include "tt.h"
#include "uci.h"

namespace {

// Captures and queen promotions: the moves quiescence search looks at.
bool is_tactical(const Position& pos, Move m) {
    return pos.piece_on(m.to()) != NO_PIECE || m.type() == EN_PASSANT
        || (m.type() == PROMOTION && m.promotion_type() == QUEEN);
}

// Move ordering. Alpha-beta prunes most when the best move is tried first:
// 1. the best move stored in the transposition table for this position,
// 2. the move from the previous iteration's best line,
// 3. captures that don't lose material (SEE), most valuable victim first, then least
//    valuable attacker (MVV-LVA),
// 4. queen promotions,
// 5. killers: quiet moves that caused a cutoff at the same ply elsewhere in the tree,
// 6. the countermove: the quiet move that last refuted the opponent's previous move,
// 7. other quiet moves, by history: how often each move caused cutoffs, on its own and
//    as a reply to the last two moves (continuation history),
// 8. captures that lose material.
// Only generated moves are scored, so a stale table move can never be played.
struct QuietOrdering {
    const Move* killers = nullptr;
    Move        counter = Move::none();
    // Null in quiescence, where the only quiet moves are check evasions.
    const int (*history)[SQUARE_NB] = nullptr;
    const int16_t* cont[2]          = {};  // continuation history rows, or null
};

void score_moves(const Position& pos, const MoveList& list, int* scores, Move ttMove, Move hint,
                 const QuietOrdering& q = {}) {
    for (int i = 0; i < list.size(); ++i) {
        const Move m = list.moves[i];
        if (m == ttMove)
            scores[i] = 2'000'000;
        else if (m == hint)
            scores[i] = 1'000'000;
        else if (pos.piece_on(m.to()) != NO_PIECE || m.type() == EN_PASSANT) {
            const PieceType victim = m.type() == EN_PASSANT ? PAWN : type_of(pos.piece_on(m.to()));
            scores[i] = (pos.see_ge(m, 0) ? 100'000 : -100'000) + 10 * victim
                      - type_of(pos.piece_on(m.from()));
        } else if (m.type() == PROMOTION && m.promotion_type() == QUEEN)
            scores[i] = 90'000;
        else if (q.killers && m == q.killers[0])
            scores[i] = 80'002;
        else if (q.killers && m == q.killers[1])
            scores[i] = 80'001;
        else if (m == q.counter)
            scores[i] = 80'000;
        else if (q.history) {  // within +-3 * HistoryMax
            const int pieceTo = pos.piece_on(m.from()) * SQUARE_NB + m.to();
            scores[i]         = q.history[m.from()][m.to()];
            for (const int16_t* row : q.cont)
                if (row)
                    scores[i] += row[pieceTo];
        } else
            scores[i] = 0;
    }
}

// History scores move toward +-HistoryMax and saturate there ("gravity"), so recent
// results count more than old ones.
constexpr int HistoryMax = 16384;

template<typename T>
void add_history(T& h, int bonus) {
    h += bonus - h * std::abs(bonus) / HistoryMax;
}

// Late move reductions by [depth][move number]: deeper nodes and later moves are
// reduced more, growing like log(depth) * log(moves).
struct ReductionTable {
    int r[MAX_PLY][MAX_MOVES];
    ReductionTable() {
        for (int d = 0; d < MAX_PLY; ++d)
            for (int n = 0; n < MAX_MOVES; ++n)
                r[d][n] = d && n ? int(0.75 + std::log(d) * std::log(n) / 2.25) : 0;
    }
};
const ReductionTable Reductions;

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

// Mate scores count plies from the root, but a table entry can be reached at any ply:
// store them counted from the node itself, and convert back when reading.
int score_to_tt(int s, int ply) {
    return s >= MATE_BOUND ? s + ply : s <= -MATE_BOUND ? s - ply : s;
}

int score_from_tt(int s, int ply) {
    return s >= MATE_BOUND ? s - ply : s <= -MATE_BOUND ? s + ply : s;
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

Searcher::Searcher() : rng(std::random_device{}()), contHist(PIECE_TO * PIECE_TO) { clear(); }

void Searcher::clear() {
    for (auto& h : helpers)
        h->clear();
    std::fill(&killers[0][0], &killers[0][0] + MAX_PLY * 2, Move::none());
    std::fill(&history[0][0][0], &history[0][0][0] + COLOR_NB * SQUARE_NB * SQUARE_NB, 0);
    std::fill(std::begin(counterMoves), std::end(counterMoves), Move::none());
    std::fill(contHist.begin(), contHist.end(), 0);
}

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
    // A playing style may prefer another of the best lines (not when analysing).
    int line = 0;
    if (options.variety > 0 && rootPos.game_ply() < Style::VarietyPlies && !limits.infinite)
        line = Style::vary(result, options.variety, rng());
    else if (options.style != StyleKind::Objective && !limits.infinite) {
        const Style::Choice choice = Style::choose(rootPos, result, options);
        line                       = choice.line;
        if (!choice.why.empty())
            UCI::send("info string style: " + choice.why);
    }
    const RootMove& best = result.lines[line];
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

void Searcher::helper_search(const Position& pos, const SearchLimits& l, const EngineOptions& o) {
    limits  = l;
    options = o;
    think(pos, false);
}

uint64_t Searcher::total_nodes() const {
    uint64_t total = nodes;
    for (const auto& h : helpers)
        total += h->sharedNodes.load(std::memory_order_relaxed);
    return total;
}

// Iterative deepening: search depth 1, 2, 3, ... until a limit is hit. Each iteration
// orders moves for the next (best line first), and there is always a finished answer to
// fall back on when time runs out mid-iteration.
SearchResult Searcher::think(const Position& pos, bool print) {
    startTime = std::chrono::steady_clock::now();
    nodes     = 0;
    sharedNodes.store(0, std::memory_order_relaxed);
    aborted   = false;
    std::fill(std::begin(pvHint), std::end(pvHint), Move::none());
    // Killers belong to positions of the previous search; history still says something
    // about the same game, with half the weight.
    std::fill(&killers[0][0], &killers[0][0] + MAX_PLY * 2, Move::none());
    std::fill(std::begin(excludedMove), std::end(excludedMove), Move::none());
    for (int& h : std::span(&history[0][0][0], COLOR_NB * SQUARE_NB * SQUARE_NB))
        h /= 2;
    for (int16_t& h : contHist)
        h /= 2;

    Position root = pos;
    set_time_limits(root.side_to_move());
    evalStack[0] = root.in_check() ? -INF : Eval::evaluate(root);
    if (!helperId)
        TT.new_search();

    MoveList legal;
    generate_legal(root, legal);
    int scores[MAX_MOVES];
    score_moves(root, legal, scores, Move::none(), Move::none());
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

    // Lazy SMP: helper threads search the same position. They share what they find through
    // the transposition table, which fills it faster and with more variety than one thread.
    std::vector<std::thread> helperThreads;
    if (!helperId && options.threads > 1) {
        while (int(helpers.size()) < options.threads - 1) {
            helpers.push_back(std::make_unique<Searcher>());
            helpers.back()->helperId = int(helpers.size());
        }
        helpers.resize(options.threads - 1);
        SearchLimits helperLimits;
        helperLimits.depth          = limits.depth;
        EngineOptions helperOptions = options;
        helperOptions.multiPV       = 1;
        helperOptions.thinkAloud    = false;
        for (auto& h : helpers) {
            h->stopRequested = false;  // here, so a stop can't come before the helper starts
            helperThreads.emplace_back(&Searcher::helper_search, h.get(), std::cref(pos),
                                       helperLimits, helperOptions);
        }
    }

    const int maxDepth = limits.depth ? std::min(limits.depth, MAX_PLY - 1) : MAX_PLY - 1;
    // A style chooses among several good lines, so the search keeps more of them.
    const bool styled   = options.style != StyleKind::Objective
                       || (options.variety > 0 && pos.game_ply() < Style::VarietyPlies);
    const int  wantedPV = !styled || helperId ? options.multiPV : std::max(options.multiPV, Style::Lines);
    const int multiPV  = std::min<int>(wantedPV, int(rootMoves.size()));
    Move      prevBest  = Move::none();
    int       prevScore = 0;
    int       stability = 0;  // iterations in a row with the same best move

    for (int depth = 1; depth <= maxDepth; ++depth) {
        // Odd helpers skip every other depth, so the threads aren't all on the same one.
        if (helperId % 2 == 1 && depth > 1 && depth % 2 == 0 && depth < maxDepth)
            continue;
        selDepth = 0;
        for (int pvIdx = 0; pvIdx < multiPV && !aborted; ++pvIdx) {
            // Aspiration window: the score will probably be close to the last iteration's,
            // and a narrow window prunes more. If it lands outside, widen and search again.
            int delta = 25, alpha = -INF, beta = INF;
            if (depth >= 5 && pvIdx < int(result.lines.size())
                && std::abs(result.lines[pvIdx].score) < MATE_BOUND) {
                alpha = result.lines[pvIdx].score - delta;
                beta  = result.lines[pvIdx].score + delta;
            }
            while (true) {
                const int score = search_root(root, depth, pvIdx, alpha, beta);
                if (aborted)
                    break;
                if (score <= alpha) {  // fail low: worse than expected
                    beta  = (alpha + beta) / 2;
                    alpha = std::max(score - delta, -INF);
                } else if (score >= beta)  // fail high: better than expected
                    beta = std::min(score + delta, INF);
                else
                    break;
                delta += delta / 2;
            }
        }
        if (aborted)
            break;  // keep the last completed iteration

        result.lines.assign(rootMoves.begin(), rootMoves.begin() + multiPV);
        result.depth = depth;
        const RootMove& best = rootMoves[0];
        for (size_t i = 0; i < best.pv.size() && i < MAX_PLY; ++i)
            pvHint[i] = best.pv[i];

        if (print) {
            report(depth, std::min(options.multiPV, multiPV));
            if (options.thinkAloud)
                think_aloud(root, depth, prevBest, prevScore);
        }
        stability       = best.move == prevBest ? stability + 1 : 0;
        const int drop  = prevBest.is_ok() ? prevScore - best.score : 0;
        prevBest        = best.move;
        prevScore       = best.score;

        // On the clock: a proven mate won't get any better, and starting an iteration
        // past the soft limit would likely be cut off by the hard limit anyway. The soft
        // limit stretches while the search keeps changing its mind or the score falls
        // (a problem has just been seen), and shrinks once the choice has settled.
        if (softLimit) {
            if (std::abs(best.score) >= MATE_BOUND && depth > MATE - std::abs(best.score))
                break;
            constexpr double StabilityScale[] = {1.8, 1.3, 1.05, 0.9, 0.8};
            double scale = StabilityScale[std::min(stability, 4)];
            if (depth >= 6 && drop > 25)
                scale *= drop > 75 ? 1.5 : 1.25;
            if (elapsed_ms() >= int64_t(softLimit * scale))
                break;
        }
    }

    for (auto& h : helpers)
        h->stop();
    for (std::thread& t : helperThreads)
        t.join();

    if (result.lines.empty())  // aborted inside depth 1: best guess by move ordering
        result.lines = {RootMove{rootMoves[0].move, 0, {rootMoves[0].move}}};
    result.nodes = total_nodes();

    if (print && options.thinkAloud) {
        const RootMove& best = result.lines[0];
        UCI::send("info string [think] decision: " + to_san(root, best.move) + " after "
                  + std::to_string(result.nodes) + " positions in " + std::to_string(elapsed_ms())
                  + " ms, depth " + std::to_string(result.depth) + ". Expected line: "
                  + line_to_san(root, best.pv));
    }
    return result;
}

// Root moves [pvIdx, end) are searched, the first with the window [alpha, beta] and the
// rest as in negamax (PVS); the best of them lands at pvIdx. Its score is exact if it is
// inside the window; the return value is the best score, to tell. Calling this for
// pvIdx = 0, 1, 2 gives MultiPV lines.
int Searcher::search_root(Position& pos, int depth, int pvIdx, int alpha, int beta) {
    for (size_t i = pvIdx; i < rootMoves.size(); ++i)
        rootMoves[i].score = -INF;  // left from the last iteration; the order is kept

    int best = -INF;
    for (size_t i = pvIdx; i < rootMoves.size(); ++i) {
        RootMove& rm = rootMoves[i];
        movedStack[0] = pos.piece_on(rm.move.from()) * SQUARE_NB + rm.move.to();
        pos.make_move(rm.move);
        ++nodes;
        int score;
        if (i == size_t(pvIdx))
            score = -negamax(pos, depth - 1, 1, -beta, -alpha);
        else {
            score = -negamax(pos, depth - 1, 1, -alpha - 1, -alpha);
            if (score > alpha && score < beta && !aborted)
                score = -negamax(pos, depth - 1, 1, -beta, -alpha);
        }
        pos.unmake_move(rm.move);
        if (aborted)
            return 0;

        best = std::max(best, score);
        if (score > alpha) {  // otherwise only an upper bound: stays -INF and sorts behind
            alpha    = score;
            rm.score = score;
            rm.pv.assign(1, rm.move);
            rm.pv.insert(rm.pv.end(), &pvTable[1][1], &pvTable[1][pvLength[1]]);
            if (score >= beta)
                break;  // fail high: the caller searches again with a wider window
        }
    }
    std::stable_sort(rootMoves.begin() + pvIdx, rootMoves.end(),
                     [](const RootMove& a, const RootMove& b) { return a.score > b.score; });
    return best;
}

// Fail-soft alpha-beta in negamax form: every score is from the side to move's view, so
// a child's score is negated. [alpha, beta] is the window of scores that still matter:
// a move scoring >= beta means the opponent will avoid this position anyway (cutoff).
//
// Principal variation search: with good move ordering the first move is usually best, so
// the others are only asked "are you better than alpha?" with a null window
// [alpha, alpha + 1], which is much cheaper. Only a move that says yes is searched again
// with the full window to get its exact score. PV nodes (window wider than null) are the
// ones on the expected best line.
int Searcher::negamax(Position& pos, int depth, int ply, int alpha, int beta, bool canNull) {
    const bool pvNode = beta - alpha > 1;
    pvLength[ply] = ply;
    if (should_stop())
        return 0;
    if (pos.is_draw())
        return draw_score(ply);

    // Mate distance pruning: even mating right here can't beat a mate already found
    // closer to the root, and being mated right here can't be worse than one found there.
    if (!options.exact) {
        alpha = std::max(alpha, -MATE + ply);
        beta  = std::min(beta, MATE - ply - 1);
        if (alpha >= beta)
            return alpha;
    }

    // Check extension: a check must be answered and leaves few replies, so searching it
    // one ply deeper is cheap, and it stops forcing sequences from vanishing over the
    // horizon. This also keeps checks out of quiescence's stand-pat.
    const bool inCheck = pos.in_check();
    if (inCheck && !options.exact)
        ++depth;

    if (depth <= 0)
        return options.quiescence ? qsearch(pos, ply, alpha, beta) : Eval::evaluate(pos);
    if (ply >= MAX_PLY - 1)
        return Eval::evaluate(pos);

    // A stored result at least as deep as this search settles the node if its bound
    // fits the window. Not on PV nodes: the line shown to the user would be cut short.
    // A singular search (see below) asks about this position without one move, so the
    // stored result, which includes that move, doesn't apply.
    const Move excluded = excludedMove[ply];
    TTEntry    tte;
    const bool ttHit   = TT.probe(pos.key(), tte);
    const Move ttMove  = ttHit ? tte.move : Move::none();
    const int  ttScore = ttHit ? score_from_tt(tte.score, ply) : 0;
    if (ttHit && !pvNode && !options.exact && !excluded.is_ok() && tte.depth >= depth) {
        if (tte.bound() == BOUND_EXACT || (tte.bound() == BOUND_LOWER && ttScore >= beta)
            || (tte.bound() == BOUND_UPPER && ttScore <= alpha))
            return ttScore;
    }

    const Color us         = pos.side_to_move();
    const int   staticEval = inCheck ? -INF : Eval::evaluate(pos);
    evalStack[ply]         = staticEval;
    // Improving: better than at our previous turn. Then a fail high is more likely and a
    // fail low less, so prune less; when getting worse, prune and reduce more.
    const bool improving = !inCheck && ply >= 2 && staticEval > evalStack[ply - 2];

    // Internal iterative reduction: with no stored move, this node was never searched
    // before and its move ordering is poor. Search it a ply shallower; the result fills
    // the table, so the next iteration finds it ordered properly.
    if (depth >= 4 && !ttMove.is_ok() && !options.exact)
        --depth;

    // Reverse futility pruning: near the leaves, if the position is already so far above
    // beta that no realistic loss over the remaining plies brings it back, stop here.
    // The margin grows with depth because more can go wrong in more moves.
    if (!pvNode && !inCheck && !options.exact && !excluded.is_ok() && depth <= 6
        && std::abs(beta) < MATE_BOUND && staticEval - 90 * (depth - improving) >= beta)
        return staticEval;

    // Null move pruning: let the opponent move twice in a row. If a reduced search still
    // fails high, a real move would almost certainly do so too. The exception is
    // zugzwang, where passing would be the best move; it is common when only king and
    // pawns are left, so those positions are excluded. Two passes in a row prove nothing.
    if (!pvNode && !inCheck && canNull && !options.exact && !excluded.is_ok() && depth >= 3
        && (pos.pieces(us) & ~pos.pieces(PAWN, KING)) && staticEval >= beta) {
        const int R = 3 + depth / 6;
        movedStack[ply] = -1;
        pos.make_null_move();
        ++nodes;
        const int score = -negamax(pos, depth - 1 - R, ply + 1, -beta, -beta + 1, false);
        pos.unmake_null_move();
        if (aborted)
            return 0;
        if (score >= beta)
            return score >= MATE_BOUND ? beta : score;  // a mate found by passing isn't proven
    }

    // Singular extension: if the stored move is far better than every other move (a
    // search without it, at reduced depth and a margin below its score, fails low), the
    // position hinges on it, so it is searched a ply deeper. If even the others reach
    // beta, several moves refute the opponent's choice and the node is cut (multi-cut).
    int ttExtension = 0;
    if (!options.exact && depth >= 8 && ttMove.is_ok() && !excluded.is_ok()
        && tte.depth >= depth - 3 && tte.bound() != BOUND_UPPER && std::abs(ttScore) < MATE_BOUND) {
        const int singularBeta = ttScore - 2 * depth;
        excludedMove[ply]      = ttMove;
        const int score = negamax(pos, (depth - 1) / 2, ply, singularBeta - 1, singularBeta, false);
        excludedMove[ply] = Move::none();
        pvLength[ply]     = ply;  // the inner search wrote a line for this ply
        if (aborted)
            return 0;
        if (score < singularBeta)
            ttExtension = 1;
        else if (singularBeta >= beta)
            return singularBeta;
    }

    const int origAlpha = alpha;
    MoveList  list;
    generate_pseudo_legal(pos, list);
    int scores[MAX_MOVES];
    QuietOrdering ordering;
    ordering.killers = killers[ply];
    ordering.counter = ply >= 1 && movedStack[ply - 1] >= 0 ? counterMoves[movedStack[ply - 1]]
                                                             : Move::none();
    ordering.history = history[us];
    ordering.cont[0] = cont_row(ply - 1);
    ordering.cont[1] = cont_row(ply - 2);
    score_moves(pos, list, scores, ttMove, pvHint[ply], ordering);

    int  best = -INF, legal = 0, quietCount = 0;
    Move bestMove = Move::none();
    Move quiets[MAX_MOVES];  // quiet moves searched without a cutoff
    for (int i = 0; i < list.size(); ++i) {
        const Move m = pick_next(list, scores, i);
        if (m == excluded)
            continue;
        const bool quiet    = !is_tactical(pos, m);
        const int  newDepth = depth - 1 + (m == ttMove ? ttExtension : 0);
        int        hist     = 0;  // history sum of a quiet move, -3..3 * HistoryMax
        if (quiet) {
            const int pieceTo = pos.piece_on(m.from()) * SQUARE_NB + m.to();
            hist              = history[us][m.from()][m.to()];
            for (const int16_t* row : ordering.cont)
                if (row)
                    hist += row[pieceTo];
        }

        // Shallow pruning of quiet moves, once some move has kept us from being mated:
        // late move pruning (with good ordering, a quiet move this far down the list
        // almost never matters near the leaves) and futility pruning (the position is so
        // far below alpha that a quiet move won't make up the difference in time).
        if (!pvNode && !inCheck && quiet && best > -MATE_BOUND && !options.exact) {
            if (depth <= 4 && quietCount >= (3 + depth * depth) / (2 - improving))
                continue;
            if (depth <= 6 && staticEval + 100 + 100 * depth <= alpha)
                continue;
        }

        movedStack[ply] = pos.piece_on(m.from()) * SQUARE_NB + m.to();
        pos.make_move(m);
        if (pos.mover_in_check()) {
            pos.unmake_move(m);
            continue;
        }
        ++legal;
        ++nodes;
        int score;
        if (legal == 1)
            score = -negamax(pos, newDepth, ply + 1, -beta, -alpha);
        else {
            // Late move reductions: with good ordering, a quiet move this far down the
            // list rarely turns out best, so search it shallower first. If it beats
            // alpha anyway, it gets the full depth. Never reduced: captures, checks,
            // check evasions and killers.
            int r = 0;
            if (depth >= 3 && quiet && !inCheck && !options.exact && !pos.in_check()
                && m != killers[ply][0] && m != killers[ply][1]) {
                // Moves with a good record are reduced less, bad ones more.
                r = Reductions.r[depth][legal] - pvNode + !improving - hist / 8192;
                r = std::clamp(r, 0, depth - 2);
            }
            score = -negamax(pos, newDepth - r, ply + 1, -alpha - 1, -alpha);
            if (score > alpha && r > 0 && !aborted)
                score = -negamax(pos, newDepth, ply + 1, -alpha - 1, -alpha);
            if (score > alpha && score < beta && !aborted)
                score = -negamax(pos, newDepth, ply + 1, -beta, -alpha);
        }
        pos.unmake_move(m);
        if (aborted)
            return 0;

        if (score > best) {
            best = score;
            if (score > alpha) {
                alpha    = score;
                bestMove = m;
                update_pv(ply, m);
                if (alpha >= beta) {
                    if (quiet)
                        update_quiet_stats(pos, ply, depth, m, quiets, quietCount);
                    break;
                }
            }
        }
        if (quiet)
            quiets[quietCount++] = m;
    }

    if (!legal)  // checkmate (sooner is worse) or stalemate; or only the excluded move
        return excluded.is_ok() ? alpha : inCheck ? -MATE + ply : draw_score(ply);
    if (excluded.is_ok())
        return best;  // not this position's full result: don't store it

    const Bound bound = best >= beta ? BOUND_LOWER : best > origAlpha ? BOUND_EXACT : BOUND_UPPER;
    TT.store(pos.key(), bestMove, score_to_tt(best, ply), depth, bound);
    return best;
}

int16_t* Searcher::cont_row(int ply) {
    return ply >= 0 && movedStack[ply] >= 0 ? &contHist[movedStack[ply] * PIECE_TO] : nullptr;
}

// A quiet move caused a cutoff: remember it as a killer for this ply and as the answer to
// the opponent's last move, raise its histories and lower those of the quiet moves tried
// before it, which didn't.
void Searcher::update_quiet_stats(const Position& pos, int ply, int depth, Move best,
                                  const Move* quiets, int quietCount) {
    if (killers[ply][0] != best) {
        killers[ply][1] = killers[ply][0];
        killers[ply][0] = best;
    }
    if (ply >= 1 && movedStack[ply - 1] >= 0)
        counterMoves[movedStack[ply - 1]] = best;

    const Color us    = pos.side_to_move();
    const int   bonus = std::min(32 * depth * depth, 2048);
    auto        apply = [&](Move m, int b) {
        add_history(history[us][m.from()][m.to()], b);
        const int pieceTo = pos.piece_on(m.from()) * SQUARE_NB + m.to();
        for (int back : {1, 2})
            if (int16_t* row = cont_row(ply - back))
                add_history(row[pieceTo], b);
    };
    apply(best, bonus);
    for (int i = 0; i < quietCount; ++i)
        apply(quiets[i], -bonus);
}

// Quiescence search: at the end of the main search, keep playing captures until the
// position is quiet, so the eval never sees a half-finished exchange (horizon effect).
int Searcher::qsearch(Position& pos, int ply, int alpha, int beta) {
    const bool pvNode = beta - alpha > 1;
    pvLength[ply] = ply;
    if (should_stop())
        return 0;
    selDepth = std::max(selDepth, ply);
    if (ply >= MAX_PLY - 1)
        return Eval::evaluate(pos);

    // Any stored result is at least as deep as quiescence (depth 0).
    TTEntry    tte;
    const bool ttHit  = TT.probe(pos.key(), tte);
    const Move ttMove = ttHit ? tte.move : Move::none();
    if (ttHit && !pvNode && !options.exact) {
        const int s = score_from_tt(tte.score, ply);
        if (tte.bound() == BOUND_EXACT || (tte.bound() == BOUND_LOWER && s >= beta)
            || (tte.bound() == BOUND_UPPER && s <= alpha))
            return s;
    }
    const int origAlpha = alpha;

    // Stand pat: the side to move isn't forced to capture, so the static eval is a lower
    // bound. In check there is no such option: every evasion must be searched.
    const bool inCheck  = pos.in_check();
    int        best     = -INF;
    int        standPat = -INF;
    if (!inCheck) {
        best = standPat = Eval::evaluate(pos);
        if (best >= beta)
            return best;
        alpha = std::max(alpha, best);
    }

    MoveList list;
    generate_pseudo_legal(pos, list);
    int scores[MAX_MOVES];
    score_moves(pos, list, scores, ttMove, Move::none());

    int  legal    = 0;
    Move bestMove = Move::none();
    for (int i = 0; i < list.size(); ++i) {
        const Move m = pick_next(list, scores, i);
        if (!inCheck && !is_tactical(pos, m))
            continue;
        // A capture that loses material won't help a side that could stand pat instead.
        if (!inCheck && !options.exact && !pos.see_ge(m, 0))
            continue;
        // Delta pruning: even winning the piece outright, plus a margin for positional
        // gains, leaves the score below alpha. Values are the textbook ones, as in SEE.
        if (!inCheck && !options.exact && m.type() != PROMOTION) {
            constexpr int Value[PIECE_TYPE_NB] = {100, 300, 300, 500, 900, 0};
            const int     victim = m.type() == EN_PASSANT ? PAWN : type_of(pos.piece_on(m.to()));
            const int     gain   = standPat + 150 + Value[victim];
            if (gain <= alpha) {
                best = std::max(best, gain);
                continue;
            }
        }
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
                alpha    = score;
                bestMove = m;
                update_pv(ply, m);
                if (alpha >= beta)
                    break;
            }
        }
    }

    if (inCheck && !legal)
        return -MATE + ply;

    const Bound bound = best >= beta ? BOUND_LOWER : best > origAlpha ? BOUND_EXACT : BOUND_UPPER;
    TT.store(pos.key(), bestMove, score_to_tt(best, ply), 0, bound);
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
    options       = {};
    options.exact = true;
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
        const int64_t time = limits.time[us];
        const int64_t inc  = limits.inc[us];
        // Never plan to spend the reserve: under CPU load the OS can stall the engine for
        // 50-100 ms on any move, and a clock run down to a few tens of ms then flags.
        // Below the reserve only part of the increment is spent, so the clock grows back.
        const int64_t reserve = 100 + 2 * options.moveOverhead;
        const int64_t spare   = std::max<int64_t>(0, time - options.moveOverhead - reserve);
        // Without movestogo, plan as if 30 more moves are needed (sudden death).
        const int movesLeft = limits.movestogo ? std::min(limits.movestogo, 40) : 30;
        hardLimit = std::min(spare * 3 / 4 + inc / 2, time - options.moveOverhead);
        hardLimit = std::max<int64_t>(1, hardLimit);
        softLimit = std::clamp<int64_t>(spare / movesLeft + inc * 3 / 4, 1, hardLimit);
        hardLimit = std::min(hardLimit, softLimit * 4);
        // A style searches more lines and then the opponent's replies: plan for that.
        if (options.style != StyleKind::Objective)
            softLimit = std::max<int64_t>(1, softLimit * 2 / 3);
    }
}

bool Searcher::should_stop() {
    if (aborted)
        return true;
    // Reading the clock costs more than a node, so only every 1024 nodes.
    if ((nodes & 1023) == 0) {
        sharedNodes.store(nodes, std::memory_order_relaxed);
        if (stopRequested || (hardLimit && elapsed_ms() >= hardLimit))
            aborted = true;
    }
    if (limits.nodes && nodes >= limits.nodes)
        aborted = true;
    return aborted;
}

int64_t Searcher::elapsed_ms() const {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now() - startTime).count();
}

// Contempt: the side that searches (at even plies) values a draw below zero, so it plays
// on in level positions; its opponent, by the same token, values it above zero.
int Searcher::draw_score(int ply) const {
    return ply % 2 == 0 ? -options.contempt : options.contempt;
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
                  + " nodes " + std::to_string(total_nodes()) + " nps "
                  + std::to_string(total_nodes() * 1000 / (ms + 1)) + " hashfull "
                  + std::to_string(TT.hashfull()) + " time " + std::to_string(ms) + " pv" + pv);
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
