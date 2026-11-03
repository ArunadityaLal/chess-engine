#include "tune.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <random>
#include <thread>

#include "datagen.h"
#include "eval.h"
#include "position.h"
#include "uci.h"

namespace {

// The training set, flattened: position i's features are feats[begin[i] .. begin[i + 1]).
struct Dataset {
    std::vector<Eval::Feature> feats;
    std::vector<uint32_t>      begin{0};
    std::vector<uint8_t>       phase;
    std::vector<float>         result;  // from White's side: 1, 0.5, 0
    std::vector<int16_t>       score;   // search score, White's side
    size_t                     size() const { return phase.size(); }
};

int threads() { return std::max(1u, std::thread::hardware_concurrency()); }

// Runs f(first, last, thread) over [0, n) split across all cores.
template<typename F>
void parallel(size_t n, F f) {
    const int                T = threads();
    std::vector<std::thread> pool;
    for (int t = 0; t < T; ++t)
        pool.emplace_back([=, &f] { f(n * t / T, n * (t + 1) / T, t); });
    for (std::thread& th : pool)
        th.join();
}

Dataset build(const std::vector<PackedPos>& packed) {
    const int            T = threads();
    std::vector<Dataset> parts(T);
    parallel(packed.size(), [&](size_t first, size_t last, int t) {
        Dataset&                   d = parts[t];
        Position                   pos;
        std::vector<Eval::Feature> f;
        for (size_t i = first; i < last; ++i) {
            if (!pos.set(packed[i].fen()))
                continue;
            Eval::features(pos, f);
            d.feats.insert(d.feats.end(), f.begin(), f.end());
            d.begin.push_back(uint32_t(d.feats.size()));
            d.phase.push_back(uint8_t(std::min(pos.phase(), PHASE_MAX)));
            d.result.push_back(packed[i].white_result());
            d.score.push_back(packed[i].score);
        }
    });
    Dataset all;
    for (Dataset& d : parts) {
        const uint32_t base = uint32_t(all.feats.size());
        all.feats.insert(all.feats.end(), d.feats.begin(), d.feats.end());
        for (size_t i = 1; i < d.begin.size(); ++i)
            all.begin.push_back(base + d.begin[i]);
        all.phase.insert(all.phase.end(), d.phase.begin(), d.phase.end());
        all.result.insert(all.result.end(), d.result.begin(), d.result.end());
        all.score.insert(all.score.end(), d.score.begin(), d.score.end());
    }
    return all;
}

// Weights as doubles: w[2 * p] is parameter p's middlegame value, w[2 * p + 1] its endgame.
double eval_of(const Dataset& d, size_t i, const std::vector<double>& w) {
    double mg = 0, eg = 0;
    for (uint32_t j = d.begin[i]; j < d.begin[i + 1]; ++j) {
        mg += d.feats[j].coeff * w[2 * d.feats[j].index];
        eg += d.feats[j].coeff * w[2 * d.feats[j].index + 1];
    }
    return (mg * d.phase[i] + eg * (PHASE_MAX - d.phase[i])) / PHASE_MAX;
}

double sigmoid(double eval, double k) { return 1 / (1 + std::exp(-k * eval)); }

double mean_error(const Dataset& d, const std::vector<double>& w, double k,
                  const std::vector<float>& target) {
    std::vector<double> sums(threads());
    parallel(d.size(), [&](size_t first, size_t last, int t) {
        for (size_t i = first; i < last; ++i) {
            const double e = sigmoid(eval_of(d, i, w), k) - target[i];
            sums[t] += e * e;
        }
    });
    double total = 0;
    for (double s : sums)
        total += s;
    return total / d.size();
}

// The scale that best maps the current eval onto results, by golden-section search.
double fit_k(const Dataset& d, const std::vector<double>& w) {
    double       lo = 0.0001, hi = 0.05;
    const double g  = (std::sqrt(5.0) - 1) / 2;
    for (int i = 0; i < 40; ++i) {
        const double a = hi - g * (hi - lo), b = lo + g * (hi - lo);
        if (mean_error(d, w, a, d.result) < mean_error(d, w, b, d.result))
            hi = b;
        else
            lo = a;
    }
    return (lo + hi) / 2;
}

}  // namespace

bool Tune::run(const Options& o) {
    std::vector<PackedPos> packed;
    for (const std::string& file : o.data) {
        const std::string source = Datagen::source_eval(file);
        if (source.rfind("Learned", 0) != 0 && !o.allowForeign) {
            // The Learned line must only ever learn from its own games.
            UCI::send("info string tune: " + file + " was played by '"
                      + (source.empty() ? "unknown" : source)
                      + "', not the Learned eval; refusing it (allowforeign to override)");
            return false;
        }
        const std::vector<PackedPos> part = Datagen::load(file);
        packed.insert(packed.end(), part.begin(), part.end());
        UCI::send("info string tune: " + file + ": " + std::to_string(part.size()) + " positions ("
                  + source + ")");
    }
    if (packed.empty()) {
        UCI::send("info string tune: no positions");
        return false;
    }
    std::mt19937_64 rng(42);
    std::shuffle(packed.begin(), packed.end(), rng);
    if (packed.size() > o.maxPositions)
        packed.resize(o.maxPositions);

    const Dataset d = build(packed);
    packed          = {};
    UCI::send("info string tune: " + std::to_string(d.size()) + " positions, "
              + std::to_string(d.feats.size() / std::max<size_t>(d.size(), 1))
              + " features each on average, " + std::to_string(Eval::param_count()) + " weights, "
              + std::to_string(threads()) + " threads");

    const std::vector<Score> start = Eval::learned_params();
    std::vector<double>      w(2 * start.size());
    for (size_t p = 0; p < start.size(); ++p) {
        w[2 * p]     = start[p].mg;
        w[2 * p + 1] = start[p].eg;
    }

    const double k = fit_k(d, w);
    std::vector<float> target(d.size());
    for (size_t i = 0; i < d.size(); ++i)
        target[i] = float(o.lambda * d.result[i] + (1 - o.lambda) * sigmoid(d.score[i], k));
    const double startError = mean_error(d, w, k, target);
    UCI::send("info string tune: K = " + std::to_string(k * 400 / std::log(10.0))
              + ", starting error " + std::to_string(startError));

    // Adam on the full batch. The gradient of the mean squared error with respect to a
    // weight is the sum over positions of 2 (s - y) s (1 - s) k * coefficient * phase share.
    std::vector<double> m(w.size()), v(w.size());
    const double        beta1 = 0.9, beta2 = 0.999;
    for (int epoch = 1; epoch <= o.epochs; ++epoch) {
        std::vector<std::vector<double>> grads(threads(), std::vector<double>(w.size()));
        parallel(d.size(), [&](size_t first, size_t last, int t) {
            std::vector<double>& g = grads[t];
            for (size_t i = first; i < last; ++i) {
                const double s    = sigmoid(eval_of(d, i, w), k);
                const double base = 2 * (s - target[i]) * s * (1 - s) * k;
                const double mgW  = base * d.phase[i] / PHASE_MAX;
                const double egW  = base * (PHASE_MAX - d.phase[i]) / PHASE_MAX;
                for (uint32_t j = d.begin[i]; j < d.begin[i + 1]; ++j) {
                    g[2 * d.feats[j].index] += mgW * d.feats[j].coeff;
                    g[2 * d.feats[j].index + 1] += egW * d.feats[j].coeff;
                }
            }
        });
        for (size_t p = 0; p < w.size(); ++p) {
            double g = 0;
            for (const auto& part : grads)
                g += part[p];
            g /= double(d.size());
            m[p]            = beta1 * m[p] + (1 - beta1) * g;
            v[p]            = beta2 * v[p] + (1 - beta2) * g * g;
            const double mh = m[p] / (1 - std::pow(beta1, epoch));
            const double vh = v[p] / (1 - std::pow(beta2, epoch));
            w[p] -= o.lr * mh / (std::sqrt(vh) + 1e-12);
        }
        if (epoch % 50 == 0 || epoch == o.epochs)
            UCI::send("info string tune: epoch " + std::to_string(epoch) + " error "
                      + std::to_string(mean_error(d, w, k, target)));
    }

    std::vector<Score> tuned(start.size());
    for (size_t p = 0; p < start.size(); ++p)
        tuned[p] = {int(std::lround(w[2 * p])), int(std::lround(w[2 * p + 1]))};
    std::vector<uint64_t> seen(start.size());
    for (const Eval::Feature& f : d.feats)
        ++seen[f.index];
    Eval::normalize(tuned, seen);

    std::ofstream out(o.out);
    if (!out) {
        UCI::send("info string tune: can't write " + o.out);
        return false;
    }
    out << "# Texel-tuned on " << d.size() << " positions from:\n";
    for (const std::string& file : o.data)
        out << "#   " << file << " (" << Datagen::source_eval(file) << ")\n";
    out << "# error " << startError << " -> " << mean_error(d, w, k, target) << ", K "
        << k * 400 / std::log(10.0) << ", lambda " << o.lambda << "\n";
    out << Eval::learned_params_text(tuned);
    UCI::send("info string tune: wrote " + o.out);
    return true;
}
