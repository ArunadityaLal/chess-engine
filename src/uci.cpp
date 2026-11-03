#include "uci.h"

#include <algorithm>
#include <cctype>
#include <iostream>
#include <mutex>
#include <sstream>
#include <stdexcept>

#include "datagen.h"
#include "eval.h"
#include "motifs.h"
#include "movegen.h"
#include "nnue.h"
#include "perft.h"
#include "position.h"
#include "search.h"
#include "tests.h"
#include "tune.h"
#include "tt.h"

namespace {

std::mutex outputMutex;

// Eval=NNUE may arrive before NNUEFile: remember it and switch once a network loads.
bool wantNNUE = false;

// position startpos|fen <fen> [moves <m1> <m2> ...]   (moves in UCI or SAN)
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
        Move m = parse_uci_move(pos, token);
        if (!m.is_ok())
            m = parse_san(pos, token);  // an extension: lets tools feed PGN moves directly
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
        if (name == "hash")
            TT.resize(std::clamp(std::stoi(value), 1, 65536));
        else if (name == "threads")
            options.threads = std::clamp(std::stoi(value), 1, 256);
        else if (name == "multipv")
            options.multiPV = std::clamp(std::stoi(value), 1, 64);
        else if (name == "move overhead")
            options.moveOverhead = std::clamp(std::stoi(value), 0, 5000);
        else if (name == "variety")
            options.variety = std::clamp(std::stoi(value), 0, 100);
        else if (name == "contempt")
            options.contempt = std::clamp(std::stoi(value), -200, 200);
        else if (name == "style") {
            if (value == "Objective")      options.style = StyleKind::Objective;
            else if (value == "Creative")  options.style = StyleKind::Creative;
            else if (value == "Practical") options.style = StyleKind::Practical;
            else throw std::invalid_argument(value);
        } else if (name == "thinkaloud")
            options.thinkAloud = on;
        else if (name == "randommover")
            options.randomMover = on;
        else if (name == "eval") {
            if (value != "PeSTO" && value != "Learned" && value != "NNUE")
                throw std::invalid_argument(value);
            wantNNUE = value == "NNUE";
            if (wantNNUE && !NNUE::loaded()) {
                UCI::send("info string NNUE selected: load a network with NNUEFile (until then: Learned)");
                Eval::use(Eval::Kind::Learned);
            } else
                Eval::use(value == "PeSTO" ? Eval::Kind::PeSTO
                          : value == "NNUE" ? Eval::Kind::NNUE
                                            : Eval::Kind::Learned);
            pos.refresh_eval();
        } else if (name == "nnuefile") {
            std::string error;
            if (value.empty() || value == "<empty>")
                return;
            if (!NNUE::load(value, error))
                UCI::send("info string NNUEFile not loaded: " + error);
            else {
                UCI::send("info string network loaded: " + value);
                if (wantNNUE)
                    Eval::use(Eval::Kind::NNUE);
                pos.refresh_eval();
            }
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

// datagen [games N] [nodes N] [randomplies N] [seed N] out <file.bin>
// Self-play training data with the current Eval/EvalFile.
void cmd_datagen(std::istringstream& is) {
    Datagen::Options o;
    std::string      token;
    while (is >> token) {
        if (token == "games")            is >> o.games;
        else if (token == "nodes")       is >> o.nodes;
        else if (token == "randomplies") is >> o.randomPlies;
        else if (token == "seed")        is >> o.seed;
        else if (token == "out")         is >> o.out;
    }
    if (o.out.empty())
        UCI::send("info string usage: datagen [games N] [nodes N] [randomplies N] [seed N] out <file>");
    else
        Datagen::run(o);
}

// tune [epochs N] [lr X] [lambda X] [positions N] [allowforeign] out <file> data <file>...
// Texel-tunes the Learned eval, starting from the current EvalFile.
void cmd_tune(std::istringstream& is) {
    Tune::Options o;
    std::string   token;
    while (is >> token) {
        if (token == "epochs")            is >> o.epochs;
        else if (token == "lr")           is >> o.lr;
        else if (token == "lambda")       is >> o.lambda;
        else if (token == "positions")    is >> o.maxPositions;
        else if (token == "allowforeign") o.allowForeign = true;
        else if (token == "out")          is >> o.out;
        else if (token == "data")
            while (is >> token)
                o.data.push_back(token);
    }
    if (o.out.empty() || o.data.empty())
        UCI::send("info string usage: tune [epochs N] [lr X] [lambda X] [positions N] "
                  "[allowforeign] out <file> data <file>...");
    else
        Tune::run(o);
}

// datadump <file.bin> [N]: the first N training positions as "FEN | score | result".
void cmd_datadump(std::istringstream& is) {
    std::string file;
    size_t      n = 10;
    is >> file >> n;
    UCI::send("info string source: " + Datagen::source_eval(file));
    for (const PackedPos& p : Datagen::load(file, n))
        UCI::send(p.fen() + " | " + std::to_string(p.score) + " | "
                  + (p.result == 2 ? "1-0" : p.result == 1 ? "1/2" : "0-1"));
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
                        || cmd == "bench" || cmd == "threats" || cmd == "datagen"
                        || cmd == "tune" || cmd == "quit";
    if (needsIdle) {
        searcher.stop();
        searcher.wait();
    }

    if (cmd == "uci")
        UCI::send("id name engine\n"
                  "id author Arunaditya Lal\n"
                  "option name Threads type spin default 1 min 1 max 256\n"
                  "option name Hash type spin default 16 min 1 max 65536\n"
                  "option name MultiPV type spin default 1 min 1 max 64\n"
                  "option name Move Overhead type spin default 30 min 0 max 5000\n"
                  "option name ThinkAloud type check default false\n"
                  "option name Contempt type spin default 0 min -200 max 200\n"
                  "option name Variety type spin default 0 min 0 max 100\n"
                  "option name Style type combo default Objective var Objective var Creative var Practical\n"
                  "option name RandomMover type check default false\n"
                  "option name Eval type combo default PeSTO var PeSTO var Learned var NNUE\n"
                  "option name EvalFile type string default <empty>\n"
                  "option name NNUEFile type string default <empty>\n"
                  "uciok");
    else if (cmd == "isready")
        UCI::send("readyok");
    else if (cmd == "setoption")
        cmd_setoption(options, pos, is);
    else if (cmd == "ucinewgame") {
        pos.set(StartFEN);
        TT.clear();
        searcher.clear();
    }
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
    } else if (cmd == "fen")
        UCI::send(pos.fen());
    else if (cmd == "sanline") {  // sanline <uci moves>: the line in SAN, from this position
        Position    p = pos;
        std::string token, out;
        while (is >> token) {
            const Move m = parse_uci_move(p, token);
            if (!m.is_ok())
                break;
            out += (out.empty() ? "" : " ") + to_san(p, m);
            p.make_move(m);
        }
        UCI::send("info string san " + out);
    } else if (cmd == "motifs") {  // motifs <move>: tactical patterns of a move here
        std::string token;
        is >> token;
        Move m = parse_uci_move(pos, token);
        if (!m.is_ok())
            m = parse_san(pos, token);
        std::string out;
        if (m.is_ok())
            for (const Motifs::Motif& f : Motifs::find(pos, m))
                out += (out.empty() ? "" : "; ") + f.name + (f.detail.empty() ? "" : ": " + f.detail);
        UCI::send("info string motifs " + (m.is_ok() ? out : "illegal move"));
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
        int depth = 13;  // deep enough that the pruning near the leaves and above shows up
        is >> depth;
        run_bench(depth);
    } else if (cmd == "datagen")
        cmd_datagen(is);
    else if (cmd == "tune")
        cmd_tune(is);
    else if (cmd == "datadump")
        cmd_datadump(is);
    else if (cmd == "test") {
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
