// NNUE trainer for the Learned line: fits the engine's network (src/nnue.h) to positions
// from the Learned line's own self-play (datagen .bin files), nothing else.
//
//   make trainer
//   build/release/nnue-train --data build/data/learned/gen5/*.bin --out evals/nnue/net1.nnue
//
// Options: --epochs N (20)  --batch N (16384)  --lr X (0.001)  --wdl X (0.4: weight of the
// game result vs the search score in the target)  --threads N (all)  --max-positions N
// --init <net.nnue> (start from a network, e.g. the previous generation)  --allow-foreign
//
// Model, in floats: acc = b1 + sum of w1 rows of the active inputs (one accumulator per
// side), a = clamp(acc, 0, 1)^2 (SCReLU), out = a_stm . wo[0:H] + a_other . wo[H:2H] + bo,
// eval = 400 * out centipawns. Loss: mean squared error of sigmoid(out) against the target.
// Saved quantized exactly as the engine reads it (QA = 255, QB = 64).

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr int INPUTS = 768, HIDDEN = 256, QA = 255, QB = 64;
// Weight clipping keeps the engine's integer arithmetic exact: an accumulator is at most
// 33 rows of 1.98 * 255 (fits int16), an output sum at most 256 * 255^2 * 127 (fits int32).
constexpr float CLIP = 1.98f;

// Same layout as PackedPos in src/datagen.h.
struct Packed {
    uint64_t occupancy;
    uint8_t  pieces[16];
    int16_t  score;
    uint8_t  result;
    uint8_t  stm;
    uint8_t  reserved[4];
};
static_assert(sizeof(Packed) == 32);

// Parameter layout in one vector, so the optimizer treats them all alike.
constexpr size_t W1 = 0, B1 = W1 + size_t(INPUTS) * HIDDEN, WO = B1 + HIDDEN, BO = WO + 2 * HIDDEN,
                 PARAMS = BO + 1;

struct Options {
    std::vector<std::string> data;
    std::string              out, init;
    int                      epochs = 20, batch = 16384, threads = 0;
    double                   lr = 0.001, wdl = 0.4;
    size_t                   maxPositions = 0;
    bool                     allowForeign = false;
};

// Active inputs of a position from each side (same indexing as NNUE::feature).
int features(const Packed& p, int feats[2][32]) {
    int n = 0;
    for (uint64_t b = p.occupancy; b; b &= b - 1, ++n) {
        const int sq    = __builtin_ctzll(b);
        const int pc    = (p.pieces[n / 2] >> (4 * (n % 2))) & 15;
        const int color = pc / 6, type = pc % 6;
        for (int side = 0; side < 2; ++side)
            feats[side][n] = ((color == side ? 0 : 6) + type) * 64 + (side == 0 ? sq : sq ^ 56);
    }
    return n;
}

float sigmoid(float x) { return 1 / (1 + std::exp(-x)); }

float target_of(const Packed& p, double wdl) {
    const float result = p.stm == 0 ? p.result / 2.0f : 1 - p.result / 2.0f;
    const float score  = p.stm == 0 ? p.score : -p.score;
    return float(wdl * result + (1 - wdl) * sigmoid(score / 400.0f));
}

// Forward pass; with grad, also adds this position's gradient (scaled by `scale`).
// Returns the squared error.
float run(const std::vector<float>& w, const Packed& p, double wdl, float* grad, float scale) {
    int       feats[2][32];
    const int n = features(p, feats);
    float     acc[2][HIDDEN];
    for (int side = 0; side < 2; ++side) {
        std::copy(&w[B1], &w[B1] + HIDDEN, acc[side]);
        for (int j = 0; j < n; ++j) {
            const float* row = &w[W1 + size_t(feats[side][j]) * HIDDEN];
            for (int i = 0; i < HIDDEN; ++i)
                acc[side][i] += row[i];
        }
    }
    const int us = p.stm, them = 1 - p.stm;
    float     out = w[BO];
    for (int i = 0; i < HIDDEN; ++i) {
        const float a = std::clamp(acc[us][i], 0.0f, 1.0f), b = std::clamp(acc[them][i], 0.0f, 1.0f);
        out += a * a * w[WO + i] + b * b * w[WO + HIDDEN + i];
    }
    const float pred = sigmoid(out), t = target_of(p, wdl), err = pred - t;
    if (!grad)
        return err * err;

    const float g = 2 * err * pred * (1 - pred) * scale;
    grad[BO] += g;
    for (int half = 0; half < 2; ++half) {
        const int side = half == 0 ? us : them;
        for (int i = 0; i < HIDDEN; ++i) {
            const float x = acc[side][i];
            const float a = std::clamp(x, 0.0f, 1.0f);
            grad[WO + half * HIDDEN + i] += g * a * a;
            acc[side][i] = x > 0 && x < 1 ? g * w[WO + half * HIDDEN + i] * 2 * x : 0;  // d acc
        }
        for (int i = 0; i < HIDDEN; ++i)
            grad[B1 + i] += acc[side][i];
        for (int j = 0; j < n; ++j) {
            float* row = &grad[W1 + size_t(feats[side][j]) * HIDDEN];
            for (int i = 0; i < HIDDEN; ++i)
                row[i] += acc[side][i];
        }
    }
    return err * err;
}

std::string source_of(const std::string& bin) {
    std::ifstream in(bin + ".meta");
    std::string   line, eval;
    while (std::getline(in, line))
        if (line.rfind("eval ", 0) == 0)
            eval = eval.empty() || eval == line.substr(5) ? line.substr(5) : "mixed";
    return eval;
}

bool save(const std::vector<float>& w, const std::string& path) {
    std::ofstream out(path, std::ios::binary);
    auto put = [&](auto v) { out.write(reinterpret_cast<const char*>(&v), sizeof v); };
    auto q   = [](float x, int scale) { return int16_t(std::lround(std::clamp(x, -CLIP, CLIP) * scale)); };
    out.write("ENNUE1", 6);
    put(int32_t(HIDDEN));
    for (size_t i = W1; i < B1; ++i) put(q(w[i], QA));
    for (size_t i = B1; i < WO; ++i) put(q(w[i], QA));
    for (size_t i = WO; i < BO; ++i) put(q(w[i], QB));
    put(int32_t(std::lround(w[BO] * QA * QB)));
    return bool(out);
}

bool load_init(std::vector<float>& w, const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    char          magic[6];
    int32_t       hidden;
    if (!in.read(magic, 6) || std::memcmp(magic, "ENNUE1", 6) || !in.read(reinterpret_cast<char*>(&hidden), 4)
        || hidden != HIDDEN)
        return false;
    auto get16 = [&] { int16_t v = 0; in.read(reinterpret_cast<char*>(&v), 2); return v; };
    for (size_t i = W1; i < B1; ++i) w[i] = get16() / float(QA);
    for (size_t i = B1; i < WO; ++i) w[i] = get16() / float(QA);
    for (size_t i = WO; i < BO; ++i) w[i] = get16() / float(QB);
    int32_t bo = 0;
    in.read(reinterpret_cast<char*>(&bo), 4);
    w[BO] = bo / float(QA * QB);
    return bool(in);
}

template<typename F>
void parallel(int threads, F f) {
    std::vector<std::thread> pool;
    for (int t = 0; t < threads; ++t)
        pool.emplace_back(f, t);
    for (auto& th : pool)
        th.join();
}

}  // namespace

int main(int argc, char* argv[]) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        const std::string a    = argv[i];
        auto              next = [&] { return i + 1 < argc ? std::string(argv[++i]) : std::string(); };
        if (a == "--data")
            while (i + 1 < argc && std::string(argv[i + 1]).rfind("--", 0) != 0)
                o.data.push_back(argv[++i]);
        else if (a == "--out") o.out = next();
        else if (a == "--init") o.init = next();
        else if (a == "--epochs") o.epochs = std::stoi(next());
        else if (a == "--batch") o.batch = std::stoi(next());
        else if (a == "--lr") o.lr = std::stod(next());
        else if (a == "--wdl") o.wdl = std::stod(next());
        else if (a == "--threads") o.threads = std::stoi(next());
        else if (a == "--max-positions") o.maxPositions = std::stoull(next());
        else if (a == "--allow-foreign") o.allowForeign = true;
        else {
            std::cerr << "unknown option " << a << "\n";
            return 1;
        }
    }
    if (o.data.empty() || o.out.empty()) {
        std::cerr << "usage: nnue-train --data <file.bin>... --out <net.nnue> [options]\n";
        return 1;
    }
    const int T = o.threads ? o.threads : std::max(1u, std::thread::hardware_concurrency());

    std::vector<Packed> data;
    for (const std::string& file : o.data) {
        const std::string source = source_of(file);
        if (source.rfind("Learned", 0) != 0 && !o.allowForeign) {
            std::cerr << file << " was played by '" << (source.empty() ? "unknown" : source)
                      << "', not the Learned line; refusing it (--allow-foreign to override)\n";
            return 1;
        }
        std::ifstream in(file, std::ios::binary | std::ios::ate);
        const size_t  n = size_t(in.tellg()) / sizeof(Packed);
        in.seekg(0);
        const size_t old = data.size();
        data.resize(old + n);
        in.read(reinterpret_cast<char*>(&data[old]), std::streamsize(n * sizeof(Packed)));
        std::cout << file << ": " << n << " positions (" << source << ")\n";
    }
    std::mt19937_64 rng(1);
    std::shuffle(data.begin(), data.end(), rng);
    if (o.maxPositions && data.size() > o.maxPositions)
        data.resize(o.maxPositions);
    const size_t validation = std::min<size_t>(std::max<size_t>(data.size() / 100, 1000), data.size() / 2);
    const size_t train      = data.size() - validation;
    if (train < size_t(o.batch)) {
        std::cerr << "not enough positions (" << data.size() << ") for a batch of " << o.batch << "\n";
        return 1;
    }

    std::vector<float> w(PARAMS);
    std::normal_distribution<float> init(0, 1);
    for (size_t i = W1; i < B1; ++i) w[i] = init(rng) / std::sqrt(float(INPUTS) / 8);
    for (size_t i = WO; i < BO; ++i) w[i] = init(rng) / std::sqrt(float(HIDDEN));
    if (!o.init.empty() && !load_init(w, o.init)) {
        std::cerr << "can't read " << o.init << "\n";
        return 1;
    }
    std::cout << train << " training and " << validation << " validation positions, " << PARAMS
              << " weights, " << T << " threads\n";

    auto validation_loss = [&] {
        std::vector<double> sums(T);
        parallel(T, [&](int t) {
            for (size_t i = train + t; i < data.size(); i += T)
                sums[t] += run(w, data[i], o.wdl, nullptr, 0);
        });
        double s = 0;
        for (double x : sums) s += x;
        return s / validation;
    };
    std::printf("start: validation loss %.6f\n", validation_loss());

    // Adam. Learning rate drops tenfold for the last quarter of the epochs.
    std::vector<float>              m(PARAMS), v(PARAMS);
    std::vector<std::vector<float>> grads(T, std::vector<float>(PARAMS));
    const float                     beta1 = 0.9f, beta2 = 0.999f;
    long                            step  = 0;
    for (int epoch = 1; epoch <= o.epochs; ++epoch) {
        const float lr = float(o.lr * (epoch > o.epochs * 3 / 4 ? 0.1 : 1.0));
        std::shuffle(data.begin(), data.begin() + long(train), rng);
        double trainLoss = 0;
        for (size_t start = 0; start + o.batch <= train; start += o.batch) {
            std::vector<double> losses(T);
            parallel(T, [&](int t) {
                std::vector<float>& g = grads[t];
                std::fill(g.begin(), g.end(), 0.0f);
                for (size_t i = start + t; i < start + o.batch; i += T)
                    losses[t] += run(w, data[i], o.wdl, g.data(), 1.0f / o.batch);
            });
            for (double l : losses) trainLoss += l;
            ++step;
            const float c1 = 1 - std::pow(beta1, float(step)), c2 = 1 - std::pow(beta2, float(step));
            parallel(T, [&](int t) {
                for (size_t p = PARAMS * t / T; p < PARAMS * (t + 1) / T; ++p) {
                    float g = 0;
                    for (int k = 0; k < T; ++k) g += grads[k][p];
                    m[p] = beta1 * m[p] + (1 - beta1) * g;
                    v[p] = beta2 * v[p] + (1 - beta2) * g * g;
                    w[p] -= lr * (m[p] / c1) / (std::sqrt(v[p] / c2) + 1e-8f);
                    if (p < BO)  // all but the output bias
                        w[p] = std::clamp(w[p], -CLIP, CLIP);
                }
            });
        }
        const size_t batches = train / o.batch;
        std::printf("epoch %d/%d: train loss %.6f, validation loss %.6f, lr %g\n", epoch, o.epochs,
                    trainLoss / (batches * o.batch), validation_loss(), lr);
        std::fflush(stdout);
        if (!save(w, o.out)) {
            std::cerr << "can't write " << o.out << "\n";
            return 1;
        }
    }
    std::cout << "saved " << o.out << "\n";
    return 0;
}
