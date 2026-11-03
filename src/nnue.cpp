#include "nnue.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <memory>
#include <vector>

namespace NNUE {

bool enabled = false;

namespace {

struct Network {
    alignas(64) int16_t w1[INPUTS][HIDDEN];
    alignas(64) int16_t b1[HIDDEN];
    alignas(64) int16_t wo[2 * HIDDEN];
    int32_t bo;
};

std::unique_ptr<Network> Net;
std::string              NetFile;

template<typename T>
bool read(std::istream& in, T* data, size_t count) {
    return bool(in.read(reinterpret_cast<char*>(data), std::streamsize(sizeof(T) * count)));
}

}  // namespace

bool load(const std::string& path, std::string& error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        error = "can't open " + path;
        return false;
    }
    if (!load(in, error))
        return false;
    NetFile = path;
    return true;
}

bool load(std::istream& in, std::string& error) {
    char    magic[6];
    int32_t hidden = 0;
    if (!read(in, magic, 6) || std::memcmp(magic, "ENNUE1", 6) || !read(in, &hidden, 1)) {
        error = "not a network file";
        return false;
    }
    if (hidden != HIDDEN) {
        error = "network has " + std::to_string(hidden) + " hidden units, this engine "
              + std::to_string(HIDDEN);
        return false;
    }
    auto net = std::make_unique<Network>();
    if (!read(in, &net->w1[0][0], INPUTS * HIDDEN) || !read(in, net->b1, HIDDEN)
        || !read(in, net->wo, 2 * HIDDEN) || !read(in, &net->bo, 1)) {
        error = "network file is truncated";
        return false;
    }
    Net = std::move(net);
    NetFile.clear();
    return true;
}

bool        loaded() { return bool(Net); }
std::string file() { return NetFile; }

void reset(Accumulator& acc) {
    for (Color p : {WHITE, BLACK})
        std::copy(std::begin(Net->b1), std::end(Net->b1), acc.v[p]);
}

void add(Accumulator& acc, Piece pc, Square s) {
    for (Color p : {WHITE, BLACK}) {
        const int16_t* w = Net->w1[feature(p, pc, s)];
        for (int i = 0; i < HIDDEN; ++i)
            acc.v[p][i] += w[i];
    }
}

void remove(Accumulator& acc, Piece pc, Square s) {
    for (Color p : {WHITE, BLACK}) {
        const int16_t* w = Net->w1[feature(p, pc, s)];
        for (int i = 0; i < HIDDEN; ++i)
            acc.v[p][i] -= w[i];
    }
}

void move(Accumulator& acc, Piece pc, Square from, Square to) {
    for (Color p : {WHITE, BLACK}) {
        const int16_t* sub = Net->w1[feature(p, pc, from)];
        const int16_t* add = Net->w1[feature(p, pc, to)];
        for (int i = 0; i < HIDDEN; ++i)
            acc.v[p][i] += add[i] - sub[i];
    }
}

// Output layer with SCReLU (clamp to [0, 1], then square). Multiplying v * w first keeps
// the product within int16 range before the second factor of v.
int evaluate(const Accumulator& acc, Color stm) {
    int64_t sum = 0;
    for (int half = 0; half < 2; ++half) {
        const int16_t* v = acc.v[half == 0 ? stm : ~stm];
        const int16_t* w = Net->wo + half * HIDDEN;
        int32_t        s = 0;  // at most 256 * 255 * 255 * 127 < 2^31 (weights are clipped)
        for (int i = 0; i < HIDDEN; ++i) {
            const int32_t x = std::clamp<int32_t>(v[i], 0, QA);
            s += x * (x * w[i]);
        }
        sum += s;
    }
    return int((sum / QA + Net->bo) * SCALE / (QA * QB));
}

}  // namespace NNUE
