#include "tt.h"

#include <algorithm>

TranspositionTable TT;

TTEntry TranspositionTable::Slot::read() const {
    const uint64_t d = data.load(std::memory_order_relaxed);
    const uint64_t k = keyXorData.load(std::memory_order_relaxed) ^ d;
    TTEntry        e;
    e.key      = k;
    e.move     = Move(uint16_t(d));
    e.score    = int16_t(uint16_t(d >> 16));
    e.depth    = uint8_t(d >> 32);
    e.genBound = uint8_t(d >> 40);
    return e;
}

void TranspositionTable::Slot::write(const TTEntry& e) {
    const uint64_t d = uint64_t(e.move.raw()) | uint64_t(uint16_t(e.score)) << 16
                     | uint64_t(e.depth) << 32 | uint64_t(e.genBound) << 40;
    keyXorData.store(e.key ^ d, std::memory_order_relaxed);
    data.store(d, std::memory_order_relaxed);
}

void TranspositionTable::resize(size_t mb) {
    count   = std::max<size_t>(1, mb * 1024 * 1024 / sizeof(Bucket));
    buckets = std::make_unique<Bucket[]>(count);
    generation = 0;
}

void TranspositionTable::clear() {
    for (size_t i = 0; i < count; ++i)
        for (Slot& s : buckets[i].slots) {
            s.keyXorData.store(0, std::memory_order_relaxed);
            s.data.store(0, std::memory_order_relaxed);
        }
    generation = 0;
}

bool TranspositionTable::probe(Key key, TTEntry& out) const {
    for (const Slot& s : bucket(key).slots) {
        const TTEntry e = s.read();
        if (e.key == key && e.bound() != BOUND_NONE) {
            out = e;
            return true;
        }
    }
    return false;
}

// Replacement: the same position is overwritten (unless that would throw away a much
// deeper result from this search); otherwise the least valuable entry goes, where old
// generations and shallow depths are worth least.
void TranspositionTable::store(Key key, Move move, int score, int depth, Bound bound) {
    Bucket& b = bucket(key);
    TTEntry entries[BucketSize];
    for (int i = 0; i < BucketSize; ++i)
        entries[i] = b.slots[i].read();

    auto worth = [&](const TTEntry& e) {
        return e.depth - 8 * ((generation - e.generation()) & 63);
    };
    int replace = 0;
    for (int i = 0; i < BucketSize; ++i) {
        if (entries[i].key == key) {
            replace = i;
            break;
        }
        if (worth(entries[i]) < worth(entries[replace]))
            replace = i;
    }

    TTEntry e = entries[replace];
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
    b.slots[replace].write(e);
}

int TranspositionTable::hashfull() const {
    const size_t sample = std::min<size_t>(count, 1000 / BucketSize);
    int          used   = 0;
    for (size_t i = 0; i < sample; ++i)
        for (const Slot& s : buckets[i].slots) {
            const TTEntry e = s.read();
            used += e.bound() != BOUND_NONE && e.generation() == generation;
        }
    return int(used * 1000 / (sample * BucketSize));
}
