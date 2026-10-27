#pragma once

#include <cstdint>

// xorshift64* (Vigna). Deterministic, so magics and Zobrist keys are identical every run.
class PRNG {
  public:
    explicit PRNG(uint64_t seed) : s(seed) {}

    uint64_t rand() {
        s ^= s >> 12;
        s ^= s << 25;
        s ^= s >> 27;
        return s * 2685821657736338717ULL;
    }

    // Few bits set; good magic candidates.
    uint64_t sparse() { return rand() & rand() & rand(); }

  private:
    uint64_t s;
};
