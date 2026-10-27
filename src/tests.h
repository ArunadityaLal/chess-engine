#pragma once

// Self-tests: magic bitboards, FEN parsing, hashing, draw rules, the perft suite (each
// position also color-flipped), eval symmetry, mate finding, and the minimax oracle.
// Quick mode is sized for the sanitized debug build; full mode is the deep gate and
// should be run on the release build. Returns true if all pass.
bool run_tests(bool full);

// Fixed-depth search over a fixed set of positions. The total node count is a
// fingerprint: a change that should not alter search behavior must not change it.
void run_bench(int depth);
