#pragma once

#include <cstdint>
#include <vector>

#include "types.h"

// What a stored score means: the true value is at least the score (LOWER: a move failed
// high), at most the score (UPPER: every move failed low), or exactly the score.
enum Bound : uint8_t { BOUND_NONE, BOUND_UPPER, BOUND_LOWER, BOUND_EXACT };

struct TTEntry {
    Key     key;    // full key: a wrong hit can't steer the search or the analysis
    Move    move;   // best move found, or none (an all-node keeps the previous move)
    int16_t score;  // mate scores are relative to this node, see Search's score_to_tt
    uint8_t depth;
    uint8_t genBound;  // generation << 2 | bound

    Bound bound() const { return Bound(genBound & 3); }
    int   generation() const { return genBound >> 2; }
};

// Transposition table: the same position is often reached by different move orders,
// and every iteration of iterative deepening revisits the previous one's tree. Results
// are stored by hash key so they can be reused instead of searched again.
class TranspositionTable {
  public:
    TranspositionTable() { resize(16); }

    void resize(size_t mb);
    void clear();
    void new_search() { generation = (generation + 1) & 63; }  // ages old entries

    bool probe(Key key, TTEntry& out) const;
    void store(Key key, Move move, int score, int depth, Bound bound);

    int hashfull() const;  // permille of a sample of entries written by the current search

  private:
    static constexpr int BucketSize = 4;
    struct alignas(64) Bucket {
        TTEntry entries[BucketSize];
    };

    Bucket& bucket(Key key) {
        return buckets[(unsigned __int128)key * buckets.size() >> 64];
    }
    const Bucket& bucket(Key key) const {
        return buckets[(unsigned __int128)key * buckets.size() >> 64];
    }

    std::vector<Bucket> buckets;
    int                 generation = 0;
};

extern TranspositionTable TT;
