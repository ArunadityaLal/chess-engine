#include "uci.h"

#include <algorithm>
#include <cctype>
#include <iostream>
#include <mutex>
#include <sstream>
#include <stdexcept>

#include "eval.h"
#include "movegen.h"
#include "perft.h"
#include "position.h"
#include "search.h"
#include "tests.h"

namespace {

std::mutex outputMutex;

// position startpos|fen <fen> [moves <m1> <m2> ...]
void cmd_position(Position& pos, std::istringstream& is) {
    std::string token, fen;
    is >> token;
    if (token == "startpos") {
        fen = StartFEN;
        is >> token;  // "moves", if present
    } else if (token == "fen") {
        while (is >> token && token != "moves")
            fen += token + ' ';
    } else
        return;

    if (!pos.set(fen)) {
        UCI::send("info string invalid FEN: " + fen);
        return;
    }
    if (token != "moves")
        return;

    while (is >> token) {
        const Move m = parse_uci_move(pos, token);
        if (!m.is_ok()) {
            UCI::send("info string illegal move: " + token);
            return;
        }
        pos.make_move(m);
    }
}

// go [wtime <ms>] [btime <ms>] [winc <ms>] [binc <ms>] [movestogo <n>] [depth <n>]
//    [nodes <n>] [movetime <ms>] [infinite] [perft <n>]
SearchLimits parse_go(std::istringstream& is) {
    SearchLimits l;
    std::string  token;
    while (is >> token) {
        if (token == "wtime")          is >> l.time[WHITE];
        else if (token == "btime")     is >> l.time[BLACK];
        else if (token == "winc")      is >> l.inc[WHITE];
        else if (token == "binc")      is >> l.inc[BLACK];
        else if (token == "movestogo") is >> l.movestogo;
        else if (token == "depth")     is >> l.depth;
        else if (token == "nodes")     is >> l.nodes;
        else if (token == "movetime")  is >> l.movetime;
        else if (token == "infinite")  l.infinite = true;
        else if (token == "perft")     is >> l.perft;
    }
    return l;
}

// setoption name <name, may contain spaces> [value <value>]
void cmd_setoption(EngineOptions& options, Position& pos, std::istringstream& is) {
    std::string token, name, value;
    is >> token;  // "name"
    while (is >> token && token != "value")
        name += (name.empty() ? "" : " ") + token;
    while (is >> token)
        value += (value.empty() ? "" : " ") + token;

    std::transform(name.begin(), name.end(), name.begin(), ::tolower);
    const bool on = value == "true";
    try {
        if (name == "multipv")
            options.multiPV = std::clamp(std::stoi(value), 1, 64);
        else if (name == "move overhead")
            options.moveOverhead = std::clamp(std::stoi(value), 0, 5000);
        else if (name == "thinkaloud")
            options.thinkAloud = on;
        else if (name == "randommover")
            options.randomMover = on;
        else if (name == "eval") {
            if (value != "PeSTO" && value != "Learned")
                throw std::invalid_argument(value);
            Eval::use(value == "PeSTO" ? Eval::Kind::PeSTO : Eval::Kind::Learned);
            pos.refresh_eval();
        } else if (name == "evalfile") {
            std::string error;
            const std::string path = value == "<empty>" ? "" : value;
            if (!Eval::load_learned(path, error))
                UCI::send("info string EvalFile not loaded: " + error);
            else {
                pos.refresh_eval();
                UCI::send("info string learned eval: "
                          + (path.empty() ? std::string("material only") : "loaded " + path));
            }
        } else
            UCI::send("info string unknown option: " + name);
    } catch (const std::exception&) {
        UCI::send("info string bad value for " + name + ": " + value);
    }
}

// threats [depth]: "if I passed, what would my opponent do?" Searches the position with
// the turn handed over and reports the opponent's best line and what it gains.
void cmd_threats(const Position& pos, Searcher& searcher, std::istringstream& is) {
    int depth = 6;
    is >> depth;
    if (pos.in_check()) {
        UCI::send("info string threats: in check, so the threat is the check itself");
        return;
    }

    Position passed = pos;
    passed.make_null_move();
    const Color them = passed.side_to_move();

    SearchLimits limits;
    limits.depth             = depth;
    const SearchResult result = searcher.search(passed, limits, EngineOptions{}, false);
    if (result.lines.empty()) {
        UCI::send("info string threats: the opponent would have no legal moves");
        return;
    }

    // Compare with the opponent's static eval: how much better than "standing still"
    // does their best line do?
    const RootMove& t       = result.lines[0];
    const int       gain    = t.score - Eval::evaluate(passed);
    const std::string who   = them == WHITE ? "White" : "Black";
    std::string       msg   = "info string threats: if it were " + who + "'s move, " + who
                            + " would play " + line_to_san(passed, t.pv);
    if (t.score >= MATE_BOUND)
        msg += " (a forced mate)";
    else
        msg += " (" + Eval::format_score(gain) + " for " + who + " compared with doing nothing)";
    UCI::send(msg);
}

// Runs one command. Returns false on "quit".
bool execute(Position& pos, Searcher& searcher, EngineOptions& options, const std::string& line) {
    std::istringstream is(line);
    std::string        cmd;
    is >> cmd;

    // Anything that reads or changes the position (other than isready/stop) first ends a
    // running search. A well-behaved GUI sends "stop" itself; this keeps a sloppy one safe.
    const bool needsIdle = cmd == "ucinewgame" || cmd == "position" || cmd == "go"
                        || cmd == "setoption" || cmd == "perft" || cmd == "test"
                        || cmd == "bench" || cmd == "threats" || cmd == "quit";
    if (needsIdle) {
        searcher.stop();
        searcher.wait();
    }

    if (cmd == "uci")
        UCI::send("id name engine\n"
                  "id author Arunaditya Lal\n"
                  "option name MultiPV type spin default 1 min 1 max 64\n"
                  "option name Move Overhead type spin default 30 min 0 max 5000\n"
                  "option name ThinkAloud type check default false\n"
                  "option name RandomMover type check default false\n"
                  "option name Eval type combo default PeSTO var PeSTO var Learned\n"
                  "option name EvalFile type string default <empty>\n"
                  "uciok");
    else if (cmd == "isready")
        UCI::send("readyok");
    else if (cmd == "setoption")
        cmd_setoption(options, pos, is);
    else if (cmd == "ucinewgame")
        pos.set(StartFEN);
    else if (cmd == "position")
        cmd_position(pos, is);
    else if (cmd == "go") {
        const SearchLimits limits = parse_go(is);
        if (limits.perft)
            perft_divide(pos, limits.perft);
        else
            searcher.start(pos, limits, options);
    } else if (cmd == "stop")
        searcher.stop();
    else if (cmd == "quit")
        return false;
    // Debugging and teaching extensions, not part of UCI.
    else if (cmd == "d") {
        std::string board = pos.pretty();
        board.pop_back();  // trailing newline
        UCI::send(board);
    } else if (cmd == "eval") {
        std::string trace = Eval::trace(pos);
        trace.pop_back();
        UCI::send(trace);
    } else if (cmd == "evalparams") {
        std::string params = Eval::params();
        params.pop_back();
        UCI::send(params);
    } else if (cmd == "threats")
        cmd_threats(pos, searcher, is);
    else if (cmd == "perft") {
        int depth = 1;
        is >> depth;
        perft_divide(pos, depth);
    } else if (cmd == "bench") {
        int depth = 6;
        is >> depth;
        run_bench(depth);
    } else if (cmd == "test") {
        std::string mode;
        is >> mode;
        run_tests(mode == "full");
    } else if (!cmd.empty())
        UCI::send("info string unknown command: " + line);
    return true;
}

}  // namespace

void UCI::send(const std::string& text) {
    std::lock_guard<std::mutex> lock(outputMutex);
    std::cout << text << std::endl;
}

int UCI::loop(int argc, char* argv[]) {
    Position      pos;
    Searcher      searcher;
    EngineOptions options;

    if (argc > 1) {
        if (std::string(argv[1]) == "test")
            return run_tests(argc > 2 && std::string(argv[2]) == "full") ? 0 : 1;
        std::string line;
        for (int i = 1; i < argc; ++i)
            line += std::string(argv[i]) + ' ';
        execute(pos, searcher, options, line);
        return 0;
    }

    std::string line;
    while (std::getline(std::cin, line) && execute(pos, searcher, options, line)) {}
    return 0;
}
