#include "style.h"

#include <algorithm>
#include <cmath>
#include <memory>

#include "movegen.h"

namespace {

constexpr int Value[PIECE_TYPE_NB] = {100, 300, 300, 500, 900, 0};

int material(const Position& pos, Color us) {
    int m = 0;
    for (int pt = PAWN; pt < KING; ++pt)
        m += Value[pt] * (popcount(pos.pieces(us, PieceType(pt))) - popcount(pos.pieces(~us, PieceType(pt))));
    return m;
}

// Chance of winning (draws count half) at a score, logistic like the Elo formula.
double expected(int cp) { return 1 / (1 + std::pow(10.0, -cp / 400.0)); }

std::string pawns(int cp) { return Eval::format_score(cp); }

Style::Choice creative(const Position& root, const SearchResult& r) {
    const RootMove& best = r.lines[0];
    if (best.score < 150 || best.score >= MATE_BOUND)
        return {};
    Position p = root;
    if (Style::material_dip(root, best.pv) <= -200)
        return {0, "the best move is a sacrifice: " + to_san(p, best.move)};

    int chosen = -1;
    for (int i = 1; i < int(r.lines.size()); ++i) {
        const RootMove& c = r.lines[i];
        if (c.score < std::max(150, best.score - 50) || c.pv.empty())
            continue;
        if (Style::material_dip(root, c.pv) <= -200 && (chosen < 0 || c.score > r.lines[chosen].score))
            chosen = i;
    }
    if (chosen < 0)
        return {};
    const RootMove& c = r.lines[chosen];
    return {chosen, "creative: " + to_san(p, c.move) + " gives up material and still wins ("
                        + pawns(c.score) + " vs " + pawns(best.score) + " for " + to_san(p, best.move)
                        + "). Line: " + line_to_san(root, c.pv)};
}

Style::Choice practical(const Position& root, const SearchResult& r, const EngineOptions& o) {
    const RootMove& best = r.lines[0];
    if (best.score > 150 || std::abs(best.score) >= MATE_BOUND || r.lines.size() < 2)
        return {};
    const int margin = best.score < -150 ? 60 : 30;  // more risk when worse: swindle chances

    // For each near-best move, how the opponent's replies look: if only one holds, a
    // fallible opponent often misses it. The model: the opponent picks among its top
    // replies with probability falling off with how much worse each is (softmax, 80 cp).
    auto       aux    = std::make_unique<Searcher>();
    SearchLimits limits;
    limits.depth = std::max(1, r.depth - 3);
    limits.nodes = std::max<uint64_t>(20'000, r.nodes / 8);
    EngineOptions auxOptions = o;
    auxOptions.multiPV       = Style::Lines;
    auxOptions.style         = StyleKind::Objective;
    auxOptions.thinkAloud    = false;
    auxOptions.threads       = 1;

    int    chosen = 0, chosenGood = 0;
    double chosenValue = -1;
    for (int i = 0; i < int(r.lines.size()); ++i) {
        const RootMove& c = r.lines[i];
        if (c.score < best.score - margin || c.score <= -INF + 1)
            continue;
        Position p = root;
        p.make_move(c.move);
        const SearchResult replies = aux->search(p, limits, auxOptions, false);
        double             value   = expected(c.score);
        int                good    = 0;
        if (!replies.lines.empty()) {
            const int top = replies.lines[0].score;
            double    sum = 0, ev = 0;
            for (const RootMove& reply : replies.lines) {
                if (reply.score <= -INF + 1)
                    continue;
                const double w = std::exp((reply.score - top) / 80.0);
                sum += w;
                ev += w * expected(-reply.score);
                good += reply.score >= top - 50;
            }
            value = ev / sum;
        }
        if (value > chosenValue + 1e-9) {
            chosen      = i;
            chosenValue = value;
            chosenGood  = good;
        }
    }
    if (chosen == 0)
        return {};
    Position p = root;
    return {chosen, "practical: " + to_san(p, r.lines[chosen].move) + " (" + pawns(r.lines[chosen].score)
                        + " vs " + pawns(best.score) + " for " + to_san(p, best.move) + ") leaves "
                        + (chosenGood <= 1 ? "only one good reply" : std::to_string(chosenGood) + " good replies")
                        + " for the opponent"};
}

}  // namespace

// A change counts only if it lasts two plies in a row: in the middle of a trade (a piece
// taken, about to be taken back) the balance dips for one ply, which is no sacrifice.
int Style::material_dip(const Position& root, const std::vector<Move>& line, int plies) {
    Position         pos   = root;
    const Color      us    = root.side_to_move();
    const int        start = material(pos, us);
    std::vector<int> m;  // balance after each ply, relative to the start
    for (int i = 0; i < int(line.size()) && i < plies; ++i) {
        pos.make_move(line[i]);
        m.push_back(material(pos, us) - start);
    }
    int dip = 0;
    for (size_t k = 0; k + 1 < m.size(); ++k)
        dip = std::min(dip, std::max(m[k], m[k + 1]));
    return dip;
}

int Style::vary(const SearchResult& r, int margin, uint64_t random) {
    int n = 0;
    while (n < int(r.lines.size()) && r.lines[n].score >= r.lines[0].score - margin
           && r.lines[n].score > -INF + 1)
        ++n;
    return n ? int(random % n) : 0;
}

Style::Choice Style::choose(const Position& root, const SearchResult& result, const EngineOptions& o) {
    if (result.lines.size() < 1)
        return {};
    if (o.style == StyleKind::Creative)
        return creative(root, result);
    if (o.style == StyleKind::Practical)
        return practical(root, result, o);
    return {};
}
