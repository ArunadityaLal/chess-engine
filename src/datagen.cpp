#include "datagen.h"

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <random>

#include "eval.h"
#include "movegen.h"
#include "nnue.h"
#include "search.h"
#include "tt.h"
#include "uci.h"

namespace {

constexpr std::string_view PieceChars = "PNBRQKpnbrqk";

bool is_quiet(const Position& pos, Move m) {
    return pos.piece_on(m.to()) == NO_PIECE && m.type() != EN_PASSANT && m.type() != PROMOTION;
}

std::string eval_name() {
    if (Eval::active() == Eval::Kind::PeSTO)
        return "PeSTO";
    if (Eval::active() == Eval::Kind::NNUE)  // the Learned line's network stage
        return "Learned-NNUE " + NNUE::file();
    const std::string file = Eval::learned_file();
    return "Learned " + (file.empty() ? std::string("material-only") : file);
}

}  // namespace

PackedPos PackedPos::pack(const Position& pos, int whiteScore, int res) {
    PackedPos p{};
    p.occupancy = pos.pieces();
    int i       = 0;
    for (Bitboard b = p.occupancy; b; ++i) {
        const Square s = pop_lsb(b);
        p.pieces[i / 2] |= uint8_t(pos.piece_on(s) << (4 * (i % 2)));
    }
    p.score  = int16_t(std::clamp(whiteScore, -32000, 32000));
    p.result = uint8_t(res);
    p.stm    = uint8_t(pos.side_to_move());
    return p;
}

std::string PackedPos::fen() const {
    Piece board[SQUARE_NB];
    std::fill(std::begin(board), std::end(board), NO_PIECE);
    int i = 0;
    for (Bitboard b = occupancy; b; ++i) {
        const Square s = pop_lsb(b);
        board[s]       = Piece((pieces[i / 2] >> (4 * (i % 2))) & 15);
    }
    std::string fen;
    for (int r = 7; r >= 0; --r) {
        int empty = 0;
        for (int f = 0; f < 8; ++f) {
            const Piece p = board[make_square(f, r)];
            if (p == NO_PIECE) {
                ++empty;
                continue;
            }
            if (empty)
                fen += char('0' + empty);
            empty = 0;
            fen += PieceChars[p];
        }
        if (empty)
            fen += char('0' + empty);
        if (r)
            fen += '/';
    }
    return fen + (stm == WHITE ? " w - - 0 1" : " b - - 0 1");
}

// Self-play for training data. Each game starts with a few random moves (variety without
// an opening book), then both sides play the engine's choice at a fixed node count. A
// game ends by the rules, or is adjudicated once both sides agree it's decided (a big
// score for several moves in a row) or long and dead level.
void Datagen::run(const Options& o) {
    std::ofstream out(o.out, std::ios::binary | std::ios::app);
    if (!out) {
        UCI::send("info string datagen: can't write " + o.out);
        return;
    }
    {
        std::ofstream meta(o.out + ".meta", std::ios::app);
        meta << "eval " << eval_name() << "\nnodes " << o.nodes << "\nrandomplies "
             << o.randomPlies << "\nseed " << o.seed << '\n';
    }

    std::mt19937_64 rng(o.seed);
    Searcher        searcher;
    EngineOptions   engineOptions;
    SearchLimits    limits;
    limits.nodes = uint64_t(o.nodes);
    limits.depth = MAX_PLY - 1;

    const auto start     = std::chrono::steady_clock::now();
    uint64_t   positions = 0;
    int        played    = 0;
    std::vector<std::pair<PackedPos, int>> game;  // position and score; result comes last

    while (played < o.games) {
        Position pos;
        bool     ok    = true;
        const int plies = o.randomPlies + int(rng() % 2);
        for (int i = 0; i < plies && ok; ++i) {
            MoveList legal;
            generate_legal(pos, legal);
            if (!legal.size())
                ok = false;
            else
                pos.make_move(legal.moves[rng() % legal.size()]);
        }
        if (!ok)
            continue;

        TT.clear();
        searcher.clear();
        game.clear();

        // Skip openings the random moves already spoiled.
        SearchResult first = searcher.search(pos, limits, engineOptions, false);
        if (first.lines.empty() || std::abs(first.lines[0].score) > 400)
            continue;

        int result = -1, winStreak = 0, drawStreak = 0;
        for (int ply = 0; result < 0; ++ply) {
            MoveList legal;
            generate_legal(pos, legal);
            if (!legal.size()) {
                result = pos.in_check() ? (pos.side_to_move() == WHITE ? 0 : 2) : 1;
                break;
            }
            if (pos.is_draw() || ply >= 400) {
                result = 1;
                break;
            }

            const SearchResult r     = ply == 0 ? first : searcher.search(pos, limits, engineOptions, false);
            const RootMove&    best  = r.lines[0];
            const int          white = pos.side_to_move() == WHITE ? best.score : -best.score;

            // winStreak counts plies in a row with a decisive score, signed by the side.
            const int sign = white > 0 ? 1 : -1;
            winStreak  = std::abs(white) < 1500 ? 0 : winStreak * sign > 0 ? winStreak + sign : sign;
            drawStreak = ply >= 80 && std::abs(white) <= 10 ? drawStreak + 1 : 0;
            if (std::abs(winStreak) >= 6) {
                result = winStreak > 0 ? 2 : 0;
                break;
            }
            if (drawStreak >= 12) {
                result = 1;
                break;
            }

            if (!pos.in_check() && is_quiet(pos, best.move) && std::abs(best.score) < MATE_BOUND)
                game.push_back({PackedPos::pack(pos, white, 0), white});
            pos.make_move(best.move);
        }

        for (auto& [p, score] : game) {
            p.result = uint8_t(result);
            out.write(reinterpret_cast<const char*>(&p), sizeof p);
        }
        out.flush();
        positions += game.size();
        ++played;
        if (played % 10 == 0 || played == o.games) {
            const auto s = std::chrono::duration_cast<std::chrono::seconds>(
                               std::chrono::steady_clock::now() - start).count();
            UCI::send("info string datagen: " + std::to_string(played) + " games, "
                      + std::to_string(positions) + " positions, " + std::to_string(s) + " s");
        }
    }
}

std::vector<PackedPos> Datagen::load(const std::string& path, size_t limit) {
    std::ifstream          in(path, std::ios::binary | std::ios::ate);
    std::vector<PackedPos> data;
    if (!in)
        return data;
    size_t n = size_t(in.tellg()) / sizeof(PackedPos);
    if (limit)
        n = std::min(n, limit);
    data.resize(n);
    in.seekg(0);
    in.read(reinterpret_cast<char*>(data.data()), std::streamsize(n * sizeof(PackedPos)));
    return data;
}

std::string Datagen::source_eval(const std::string& binPath) {
    std::ifstream in(binPath + ".meta");
    std::string   line, eval;
    while (std::getline(in, line))
        if (line.rfind("eval ", 0) == 0) {
            if (!eval.empty() && eval != line.substr(5))
                return "mixed";
            eval = line.substr(5);
        }
    return eval;
}
