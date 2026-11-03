#pragma once

#include <string>
#include <vector>

// Texel tuning of the Learned evaluation: fits its weights so that
// sigmoid(eval) predicts the results of the games the training positions came from.
namespace Tune {

struct Options {
    std::vector<std::string> data;                    // .bin files from datagen
    std::string              out;                     // EvalFile to write
    int                      epochs       = 1000;
    double                   lr           = 1.0;      // Adam step, in centipawns
    double                   lambda       = 1.0;      // target = lambda * result + (1 - lambda) * sigmoid(score)
    size_t                   maxPositions = 3'000'000;  // random sample if there are more (memory)
    bool                     allowForeign = false;    // accept data not played by the Learned eval
};

// Starts from the current Learned weights (EvalFile). Returns false on bad input.
bool run(const Options& options);

}  // namespace Tune
