#include "tests.h"

#include <cctype>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "eval.h"
#include "movegen.h"
#include "perft.h"
#include "position.h"
#include "search.h"

namespace {

struct PerftCase {
    const char*           name;
    const char*           fen;
    std::vector<uint64_t> nodes;  // nodes[d - 1] = perft(d)
    int                   quickDepth;
    int                   fullDepth;
};

// The standard suite from https://www.chessprogramming.org/Perft_Results
const std::vector<PerftCase> PerftSuite = {
    {"startpos", "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",
     {20, 400, 8902, 197281, 4865609, 119060324}, 5, 6},
    {"kiwipete", "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1",
     {48, 2039, 97862, 4085603, 193690690}, 4, 5},
    {"position 3", "8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1",
     {14, 191, 2812, 43238, 674624, 11030083, 178633661}, 5, 7},
    {"position 4", "r3k2r/Pppp1ppp/1b3nbN/nP6/BBP1P3/q4N2/Pp1P2PP/R2Q1RK1 w kq - 0 1",
     {6, 264, 9467, 422333, 15833292, 706045033}, 4, 6},
    {"position 5", "rnbq1k1r/pp1Pbppp/2p5/8/2B5/8/PPP1NnPP/RNBQK2R w KQ - 1 8",
     {44, 1486, 62379, 2103487, 89941194}, 4, 5},
    {"position 6", "r4rk1/1pp1qppp/p1np1n2/2b1p1B1/2B1P1b1/P1NP1N2/1PP1QPPP/R4RK1 w - - 0 10",
     {46, 2079, 89890, 3894594, 164075551}, 4, 5},
};

// Perft positions plus a few ordinary middlegames and endgames.
std::vector<std::string> bench_fens() {
    std::vector<std::string> fens;
    for (const PerftCase& c : PerftSuite)
        fens.push_back(c.fen);
    fens.push_back("r1bq1rk1/pp2ppbp/2np1np1/8/3NP3/2N1BP2/PPPQ2PP/R3KB1R w KQ - 3 9");
    fens.push_back("r2q1rk1/pb1nbppp/1p2pn2/2pp4/2PP4/1P2PN2/PB1NBPPP/R2Q1RK1 w - - 0 10");
    fens.push_back("8/5pk1/6p1/8/8/6P1/5PK1/3R4 w - - 0 1");
    fens.push_back("8/8/4k3/8/2p5/8/B2P4/4K3 w - - 0 1");
    return fens;
}

int failures = 0;

void check(bool ok, const std::string& what) {
    std::cout << (ok ? "[PASS] " : "[FAIL] ") << what << std::endl;
    if (!ok)
        ++failures;
}

std::string swap_case(std::string s) {
    for (char& c : s)
        c = std::isupper(c) ? std::tolower(c) : std::toupper(c);
    return s;
}

// Mirror the board top to bottom and swap colors. Every count must stay the same,
// which catches move generation that treats white and black differently.
std::string flip_fen(const std::string& fen) {
    std::istringstream is(fen);
    std::string        placement, side, castling, ep, halfmove, fullmove;
    is >> placement >> side >> castling >> ep >> halfmove >> fullmove;

    std::istringstream       ps(placement);
    std::vector<std::string> ranks;
    for (std::string rank; std::getline(ps, rank, '/');)
        ranks.push_back(rank);

    std::string flipped;
    for (auto it = ranks.rbegin(); it != ranks.rend(); ++it)
        flipped += (flipped.empty() ? "" : "/") + swap_case(*it);

    if (ep != "-")
        ep[1] = ep[1] == '3' ? '6' : '3';

    return flipped + ' ' + (side == "w" ? "b" : "w") + ' ' + swap_case(castling) + ' ' + ep
         + ' ' + halfmove + ' ' + fullmove;
}

void play(Position& pos, std::initializer_list<const char*> moves) {
    for (const char* uci : moves)
        pos.make_move(parse_uci_move(pos, uci));
}

void test_fen() {
    for (const PerftCase& c : PerftSuite) {
        Position pos;
        check(pos.set(c.fen) && pos.fen() == c.fen, std::string("FEN round trip: ") + c.name);
    }

    Position          pos;
    const std::string before = pos.fen();
    for (const char* bad : {"",
                            "8/8/8/8/8/8/8/8 w - - 0 1",                                 // no kings
                            "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP w KQkq - 0 1",           // 7 ranks
                            "rnbqkbnr/pppppppp/9/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",  // bad digit
                            "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR x KQkq - 0 1",  // bad side
                            "4k2R/8/8/8/8/8/8/4K3 w - - 0 1"})  // black in check, white to move
        check(!pos.set(bad) && pos.fen() == before,
              std::string("rejects bad FEN \"") + bad + "\", position unchanged");
}

void test_hashing() {
    Position a, b;
    play(a, {"e2e4", "e7e5", "g1f3"});
    play(b, {"g1f3", "e7e5", "e2e4"});
    check(a.key() == b.key() && a.key() == a.compute_key(),
          "transposed move orders give the same hash");

    Position noCapture;
    play(noCapture, {"e2e4"});
    check(noCapture.ep_square() == NO_SQUARE, "no e.p. square when no pawn can capture");

    Position capture;
    capture.set("rnbqkbnr/ppp1pppp/8/8/3p4/8/PPPPPPPP/RNBQKBNR w KQkq - 0 3");
    play(capture, {"e2e4"});
    check(capture.ep_square() == E3, "e.p. square set when a pawn can capture");
}

void test_draws() {
    Position pos;
    play(pos, {"g1f3", "g8f6", "f3g1"});
    const bool early = pos.is_draw();
    play(pos, {"f6g8"});
    check(!early && pos.is_draw(), "repetition detected (knights out and back)");

    Position fifty, bishop, pawn;
    fifty.set("4k3/8/8/8/8/8/4P3/4K3 w - - 100 80");
    bishop.set("8/8/8/4k3/8/8/8/4KB2 w - - 0 1");
    pawn.set("8/8/8/4k3/8/8/4P3/4K3 w - - 0 1");
    check(fifty.is_draw(), "50-move rule");
    check(bishop.is_draw() && !pawn.is_draw(), "K+B vs K is a dead draw, K+P vs K is not");
}

int eval_of(const std::string& fen) {
    Position pos;
    pos.set(fen);
    return Eval::evaluate(pos);
}

void test_eval() {
    const std::vector<std::string> fens = bench_fens();
    std::string error;

    for (const Eval::Kind kind : {Eval::Kind::PeSTO, Eval::Kind::Learned}) {
        Eval::use(kind);
        const std::string name = kind == Eval::Kind::PeSTO ? "PeSTO" : "Learned";
        int asymmetric = 0;
        for (const std::string& fen : fens)
            asymmetric += eval_of(fen) != eval_of(flip_fen(fen));
        check(eval_of(StartFEN) == 0 && !asymmetric,
              name + " eval: start position is 0, color-symmetric on " + std::to_string(fens.size())
                  + " positions");
    }

    // Untrained, the learned eval knows piece values and nothing else.
    check(eval_of("rnbqkbnr/pppppppp/8/8/4P3/8/PPPP1PPP/RNBQKBNR b KQkq - 0 1") == 0
              && eval_of("4k3/8/8/8/8/8/8/1N2K3 w - - 0 1") == 300,
          "untrained Learned eval is material only");

    // Round trip: PeSTO's weights printed as an EvalFile and loaded as Learned must
    // evaluate every position identically.
    Eval::use(Eval::Kind::PeSTO);
    std::vector<int> pesto;
    for (const std::string& fen : fens)
        pesto.push_back(eval_of(fen));
    std::istringstream file(Eval::params());
    const bool loaded = Eval::load_learned(file, error);
    Eval::use(Eval::Kind::Learned);
    std::vector<int> learned;
    for (const std::string& fen : fens)
        learned.push_back(eval_of(fen));
    check(loaded && learned == pesto, "EvalFile round trip reproduces the weights exactly");

    std::istringstream bad("value dragon 1 2");
    check(!Eval::load_learned(bad, error) && learned.front() == eval_of(fens.front()),
          "bad EvalFile rejected (" + error + "), weights unchanged");

    Eval::load_learned("", error);
    Eval::use(Eval::Kind::PeSTO);
}

void test_search(bool full) {
    Searcher searcher;

    auto search = [&](const char* fen, int depth, int multiPV = 1) {
        Position pos;
        pos.set(fen);
        SearchLimits limits;
        limits.depth = depth;
        EngineOptions options;
        options.multiPV = multiPV;
        return searcher.search(pos, limits, options, false);
    };
    // score == INF: only the move is checked.
    auto finds = [&](const char* name, const char* fen, int depth, const char* move, int score) {
        const SearchResult r = search(fen, depth);
        const bool ok = !r.lines.empty() && to_uci(r.lines[0].move) == move
                     && (score == INF || r.lines[0].score == score);
        check(ok, std::string(name) + ": " + move + (ok ? "" : " (got " + (r.lines.empty() ? "nothing"
                  : to_uci(r.lines[0].move) + " " + std::to_string(r.lines[0].score)) + ")"));
    };

    finds("back-rank mate in 1", "6k1/5ppp/8/8/8/8/8/R5K1 w - - 0 1", 2, "a1a8", MATE - 1);
    finds("scholar's mate", "r1bqkb1r/pppp1ppp/2n2n2/4p2Q/2B1P3/8/PPPP1PPP/RNB1K1NR w KQkq - 4 4",
          2, "h5f7", MATE - 1);
    finds("mates instead of stalemating", "7k/8/6K1/8/8/8/8/5Q2 w - - 0 1", 3, "f1f8", MATE - 1);
    finds("takes a hanging queen", "4k3/8/8/3q4/8/8/3R4/4K3 w - - 0 1", 3, "d2d5", INF);

    const SearchResult mate2 = search("k7/8/2K5/8/8/8/8/7R w - - 0 1", 4);
    check(!mate2.lines.empty() && mate2.lines[0].score == MATE - 3, "K+R vs K: finds mate in 2");

    const SearchResult stalemate = search("7k/5Q2/6K1/8/8/8/8/8 b - - 0 1", 3);
    check(stalemate.lines.empty(), "stalemate: no move to play");

    const SearchResult multi = search(StartFEN, 4, 3);
    check(multi.lines.size() == 3 && multi.lines[0].score >= multi.lines[1].score
              && multi.lines[1].score >= multi.lines[2].score
              && multi.lines[0].move != multi.lines[1].move,
          "MultiPV 3 gives three distinct lines, best first");

    // Quiescence oracle, on every position one move away from a few starting points with
    // captures available. Brute force is exponential in capture sequences (with queens,
    // or many pieces, it doesn't finish), so these are small endgames.
    int qPositions = 0, qMismatches = 0;
    for (const char* fen : {"8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1",
                            "2r3k1/pp3ppp/2n1b3/3p4/3P4/2N1B3/PP3PPP/2R3K1 w - - 0 1",
                            "4k3/8/3p4/2n1p3/3PB3/4N3/8/4K3 w - - 0 1",
                            "8/5pk1/6p1/8/8/6P1/5PK1/3R4 w - - 0 1"}) {
        Position pos;
        pos.set(fen);
        MoveList moves;
        generate_legal(pos, moves);
        for (Move m : moves) {
            pos.make_move(m);
            ++qPositions;
            qMismatches += searcher.quiescence(pos, true) != searcher.quiescence(pos, false);
            pos.unmake_move(m);
        }
    }
    check(!qMismatches, "alpha-beta quiescence == brute-force quiescence on "
                            + std::to_string(qPositions) + " positions");

    // The oracle: alpha-beta must return exactly the minimax score at every depth. Deep
    // with static leaves (checks the pruning), shallow with quiescence leaves (checks how
    // the two searches fit together; full-window quiescence at every leaf is costly).
    for (const bool quiescence : {false, true}) {
        EngineOptions options;
        options.quiescence = quiescence;
        const int maxDepth = quiescence ? (full ? 2 : 1) : (full ? 4 : 3);
        int       mismatches = 0;
        const std::vector<std::string> fens = bench_fens();
        for (const std::string& fen : fens) {
            Position pos;
            pos.set(fen);
            for (int depth = 1; depth <= maxDepth; ++depth) {
                SearchLimits limits;
                limits.depth        = depth;
                const int alphaBeta = searcher.search(pos, limits, options, false).lines[0].score;
                const int reference = searcher.minimax(pos, depth, options);
                if (alphaBeta != reference) {
                    ++mismatches;
                    std::cout << "  mismatch at depth " << depth << ": " << alphaBeta
                              << " vs minimax " << reference << "  " << fen << '\n';
                }
            }
        }
        check(!mismatches, std::string("alpha-beta == minimax (") + (quiescence ? "quiescence" : "static")
                               + " leaves), depths 1-" + std::to_string(maxDepth) + " on "
                               + std::to_string(fens.size()) + " positions");
    }
}

void test_perft(bool full) {
    for (const PerftCase& c : PerftSuite)
        for (bool flipped : {false, true}) {
            // Flipped runs stay at quick depth; the originals carry the deep check.
            const int         depth = full && !flipped ? c.fullDepth : c.quickDepth;
            const std::string fen   = flipped ? flip_fen(c.fen) : c.fen;

            Position pos;
            pos.set(fen);
            const std::string before = pos.fen();  // normalized, e.g. "kqKQ" -> "KQkq"
            const Key         key    = pos.key();

            const auto     start = std::chrono::steady_clock::now();
            const uint64_t n     = perft(pos, depth);
            const auto     ms    = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - start).count();

            const uint64_t expected = c.nodes[depth - 1];
            const bool     restored = pos.fen() == before && pos.key() == key;
            std::ostringstream what;
            what << "perft " << c.name << (flipped ? " (flipped)" : "") << " depth " << depth
                 << ": " << n << (n == expected ? "" : " expected " + std::to_string(expected))
                 << (restored ? "" : " [position not restored after search]") << "  (" << ms
                 << " ms, " << n * 1000 / (ms + 1) / 1000 << "k nps)";
            check(n == expected && restored, what.str());
        }
}

}  // namespace

bool run_tests(bool full) {
    failures         = 0;
    const auto start = std::chrono::steady_clock::now();

    check(Bitboards::verify_magics(), "magic bitboards match the reference slider attacks");
    test_fen();
    test_hashing();
    test_draws();
    test_perft(full);
    test_eval();
    test_search(full);

    const auto s = std::chrono::duration_cast<std::chrono::seconds>(
                       std::chrono::steady_clock::now() - start).count();
    std::cout << '\n'
              << (failures ? std::to_string(failures) + " FAILED" : "All tests passed")
              << " (" << s << " s)" << std::endl;
    return failures == 0;
}

void run_bench(int depth) {
    Searcher     searcher;
    SearchLimits limits;
    limits.depth = depth;

    uint64_t   total = 0;
    const auto start = std::chrono::steady_clock::now();
    for (const std::string& fen : bench_fens()) {
        Position pos;
        pos.set(fen);
        const SearchResult r = searcher.search(pos, limits, EngineOptions{}, false);
        total += r.nodes;
        std::cout << std::to_string(r.nodes) << " nodes  " << to_san(pos, r.lines[0].move) << "  "
                  << fen << '\n';
    }
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - start).count();
    std::cout << "\nDepth " << depth << "\nNodes searched: " << total << "\nTime: " << ms
              << " ms\nNPS: " << total * 1000 / (ms + 1) << std::endl;
}
