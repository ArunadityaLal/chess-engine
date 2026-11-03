#pragma once

#include <string>

#include "search.h"

// Playing style: a choice among the search's best lines, made at the root after the
// search and never inside it, so the objective search stays what the SPRT tests measure.
//  Creative:  when winning, prefer a line that gives up material and still wins.
//  Practical: when level or worse, prefer a move after which the opponent has few good
//             replies, by expected score against an opponent who sometimes errs.
// Practical play only pays against fallible (human) opponents; against engines it costs.
namespace Style {

struct Choice {
    int         line = 0;  // index into SearchResult::lines
    std::string why;       // plain-language reason, empty if the objective best stays
};

constexpr int Lines = 4;  // MultiPV lines the search keeps when a style is on

Choice choose(const Position& root, const SearchResult& result, const EngineOptions& options);

// Variety without an opening book: in the first moves of a game, a random one of the
// lines scoring within `margin` of the best, so self-play games differ.
constexpr int VarietyPlies = 20;
int vary(const SearchResult& result, int margin, uint64_t random);

// Lowest material balance along the first plies of a line, relative to the start, that
// lasts at least two plies (centipawns, P 1 N 3 B 3 R 5 Q 9). Below about -200: a
// sacrifice. The coach uses the same rule to tag brilliant moves.
int material_dip(const Position& root, const std::vector<Move>& line, int plies = 6);

}  // namespace Style
