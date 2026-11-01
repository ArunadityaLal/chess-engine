#include "tt.h"

#include <algorithm>

TranspositionTable TT;

void TranspositionTable::resize(size_t mb) {
    buckets.assign(std::max<size_t>(1, mb * 1024 * 1024 / sizeof(Bucket)), Bucket{});
    generation = 0;
}

void TranspositionTable::clear() {
    std::fill(buckets.begin(), buckets.end(), Bucket{});
    generation = 0;
}

bool TranspositionTable::probe(Key key, TTEntry& out) const {
    for (const TTEntry& e : bucket(key).entries)
        if (e.key == key && e.bound() != BOUND_NONE) {
            out = e;
            return true;
        }
    return false;
}

// Replacement: the same position is overwritten (unless that would throw away a much
// deeper result from this search); otherwise the least valuable entry goes, where old
// generations and shallow depths are worth least.
void TranspositionTable::store(Key key, Move move, int score, int depth, Bound bound) {
    Bucket&  b       = bucket(key);
    TTEntry* replace = &b.entries[0];
    auto     worth   = [&](const TTEntry& e) {
        return e.depth - 8 * ((generation - e.generation()) & 63);
    };
    for (TTEntry& e : b.entries) {
        if (e.key == key) {
            replace = &e;
            break;
        }
        if (worth(e) < worth(*replace))
            replace = &e;
    }

    TTEntry& e = *replace;
    if (e.key == key) {
        if (!move.is_ok())
            move = e.move;  // a fail-low has no best move: keep the one we knew
        if (bound != BOUND_EXACT && e.generation() == generation && depth + 2 < e.depth)
            return;
    }
    e.key      = key;
    e.move     = move;
    e.score    = int16_t(score);
    e.depth    = uint8_t(depth);
    e.genBound = uint8_t(generation << 2 | bound);
}

int TranspositionTable::hashfull() const {
    const size_t sample = std::min<size_t>(buckets.size(), 1000 / BucketSize);
    int          used   = 0;
    for (size_t i = 0; i < sample; ++i)
        for (const TTEntry& e : buckets[i].entries)
            used += e.bound() != BOUND_NONE && e.generation() == generation;
    return int(used * 1000 / (sample * BucketSize));
}
