#include "perft.h"

#include <chrono>
#include <iostream>

#include "movegen.h"

uint64_t perft(Position& pos, int depth) {
    if (depth == 0)
        return 1;

    MoveList list;
    generate_pseudo_legal(pos, list);

    uint64_t nodes = 0;
    for (Move m : list) {
        pos.make_move(m);
        if (!pos.mover_in_check())
            nodes += depth == 1 ? 1 : perft(pos, depth - 1);
        pos.unmake_move(m);
    }
    return nodes;
}

uint64_t perft_divide(Position& pos, int depth) {
    const auto start = std::chrono::steady_clock::now();

    MoveList list;
    generate_legal(pos, list);

    uint64_t total = 0;
    for (Move m : list) {
        pos.make_move(m);
        const uint64_t n = depth <= 1 ? 1 : perft(pos, depth - 1);
        pos.unmake_move(m);
        total += n;
        std::cout << to_uci(m) << ": " << n << '\n';
    }

    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - start).count();
    std::cout << "\nNodes: " << total << "\nTime:  " << ms << " ms"
              << "\nNPS:   " << total * 1000 / (ms + 1) << std::endl;
    return total;
}
