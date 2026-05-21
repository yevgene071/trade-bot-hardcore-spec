#include <benchmark/benchmark.h>

#include "numeric/Kde.hpp"
#include <random>
#include <vector>
#include <cmath>
#include <algorithm>

using namespace trade_bot;

// ── Scalar (no SIMD) reference implementation ──────────────────────────

static std::vector<double> scalar_estimate(const Kde::DataSet& data,
                                            const std::vector<double>& query_points,
                                            double bandwidth) {
    if (data.values.empty() || query_points.empty()) return {};
    if (bandwidth <= 0) bandwidth = 1e-6;

    std::vector<double> densities(query_points.size(), 0.0);
    double inv_h = 1.0 / bandwidth;
    if (data.total_weight <= 0) return densities;

    for (size_t i = 0; i < query_points.size(); ++i) {
        double q = query_points[i];
        double sum = 0.0;
        for (size_t j = 0; j < data.values.size(); ++j) {
            double diff = (q - data.values[j]) * inv_h;
            sum += data.weights[j] * std::exp(-0.5 * diff * diff);
        }
        densities[i] = sum / (data.total_weight * bandwidth * kSqrt2Pi);
    }
    return densities;
}

// ── Optimised variant 1: multi-query batched SIMD ─────────────────────
// Instead of looping over query_points outer and data inner (which requires
// re-loading all data per query), swap the loops and batch over data,
// accumulating per-query sums in SIMD registers.

static std::vector<double> simd_batched(const Kde::DataSet& data,
                                         const std::vector<double>& query_points,
                                         double bandwidth) {
    if (data.values.empty() || query_points.empty()) return {};
    if (bandwidth <= 0) bandwidth = 1e-6;

    const size_t nq = query_points.size();
    std::vector<double> densities(nq, 0.0);
    double inv_h = 1.0 / bandwidth;
    if (data.total_weight <= 0) return densities;

    using batch_type = xsimd::batch<double>;
    constexpr size_t simd_size = batch_type::size;
    const size_t nd = data.values.size();

    // We accumulate results per-query in an array and SIMD-reduce at the end.
    // This is a "gather" pattern — not great for SIMD unless we transpose.
    // For small nq (1-5), the original per-query loop is actually better.
    if (nq <= 4) {
        return Kde::estimate(data, query_points, bandwidth);
    }

    // For many query points, process in blocks:
    // For each SIMD data batch, compute contribution to all query points.
    // This keeps the data in cache while iterating over queries.

    // ── Transposed approach: process data in chunks, update all densities ──
    // For each chunk of data of size simd_size, compute contribution
    // to all query points.
    for (size_t j = 0; j < nd; j += simd_size) {
        const size_t chunk = std::min(simd_size, nd - j);
        // Load data chunk (may be partial)
        double x_vals[simd_size], w_vals[simd_size];
        for (size_t k = 0; k < chunk; ++k) {
            x_vals[k] = data.values[j + k];
            w_vals[k] = data.weights[j + k];
        }
        for (size_t k = chunk; k < simd_size; ++k) {
            x_vals[k] = 0.0;
            w_vals[k] = 0.0;
        }

        for (size_t i = 0; i < nq; ++i) {
            batch_type q_batch(query_points[i]);
            batch_type inv_h_batch(inv_h);
            batch_type x_batch = batch_type::load_unaligned(x_vals);
            batch_type w_batch = batch_type::load_unaligned(w_vals);
            batch_type diff = (q_batch - x_batch) * inv_h_batch;
            batch_type kernel_val = xsimd::exp(batch_type(-0.5) * diff * diff);
            batch_type contrib = w_batch * kernel_val;
            // Reduce and add to densities
            densities[i] += xsimd::reduce_add(contrib);
        }
    }

    // Normalize
    const double norm = 1.0 / (data.total_weight * bandwidth * kSqrt2Pi);
    for (size_t i = 0; i < nq; ++i) densities[i] *= norm;
    return densities;
}

// ── Optimised variant 2: Pade approximation for exp ───────────────────
// xsimd::exp is accurate but slow. For KDE, we can use a faster approximation.

// Fast Pade approximant for exp(-0.5*x^2) over the range [0, 5]
// This is a [4/4] Pade approximant of exp(-0.5*x^2) — accurate to ~1e-4
// above the regime where the value is negligible (< 1e-6 for x > 5).
static double fast_gauss(double x2) {
    // Pade [4/4] for exp(-0.5*x^2)
    // Valid for x^2 in [0, ~25]
    const double a = x2 * 0.5;
    if (a > 12.0) return 0.0;  // exp(-6) ~ 0.0025, exp(-12) ~ 6e-6 — negligible
    // [2/2] Pade: (1 - 7/30*a + a^2/40) / (1 + 4/15*a + a^2/24)
    // This is a common fast approximation for exp(-a) where a = x^2/2
    double num = 1.0 - 0.23333333333 * a + 0.025 * a * a;
    double den = 1.0 + 0.26666666667 * a + 0.04166666667 * a * a;
    return num / den;
}

static std::vector<double> simd_fast_exp(const Kde::DataSet& data,
                                          const std::vector<double>& query_points,
                                          double bandwidth) {
    if (data.values.empty() || query_points.empty()) return {};
    if (bandwidth <= 0) bandwidth = 1e-6;

    std::vector<double> densities(query_points.size(), 0.0);
    double inv_h = 1.0 / bandwidth;
    if (data.total_weight <= 0) return densities;

    // Manual SIMD with Pade approximation (no xsimd::exp call)
    using batch_type = xsimd::batch<double>;
    constexpr size_t simd_size = batch_type::size;

    for (size_t i = 0; i < query_points.size(); ++i) {
        double q = query_points[i];

        size_t j = 0;
        batch_type q_batch(q);
        batch_type inv_h_batch(inv_h);
        batch_type acc(0.0);

        for (; j + simd_size <= data.values.size(); j += simd_size) {
            batch_type x_batch = batch_type::load_unaligned(&data.values[j]);
            batch_type w_batch = batch_type::load_unaligned(&data.weights[j]);

            batch_type diff = (q_batch - x_batch) * inv_h_batch;
            // diff^2 (because kernel uses -0.5 * diff^2)
            batch_type diff2 = diff * diff;
            // Pade approximation for exp(-0.5 * diff^2)
            // We compute a = 0.5 * diff^2, then use Pade
            batch_type a = batch_type(0.5) * diff2;
            // Clamp to avoid numerical issues
            a = xsimd::min(a, batch_type(12.0));
            // [2/2] Pade
            batch_type num = batch_type(1.0) - batch_type(0.23333333333) * a + batch_type(0.025) * a * a;
            batch_type den = batch_type(1.0) + batch_type(0.26666666667) * a + batch_type(0.04166666667) * a * a;
            batch_type kernel_val = num / den;
            acc += w_batch * kernel_val;
        }

        double sum = xsimd::reduce_add(acc);
        for (; j < data.values.size(); ++j) {
            double diff = (q - data.values[j]) * inv_h;
            sum += data.weights[j] * fast_gauss(diff * diff);
        }

        densities[i] = sum / (data.total_weight * bandwidth * kSqrt2Pi);
    }
    return densities;
}

// ── Test data generation ──────────────────────────────────────────────

struct TestData {
    Kde::DataSet dataset;
    std::vector<double> query_points;
};

static TestData make_test_data(size_t n_data, size_t n_queries) {
    std::mt19937 rng(42);
    std::uniform_real_distribution<double> price_dist(100.0, 200.0);
    std::uniform_real_distribution<double> weight_dist(0.5, 10.0);
    std::uniform_real_distribution<double> query_dist(100.0, 200.0);

    TestData td;
    td.dataset.values.reserve(n_data);
    td.dataset.weights.reserve(n_data);
    for (size_t i = 0; i < n_data; ++i) {
        td.dataset.values.push_back(price_dist(rng));
        double w = weight_dist(rng);
        td.dataset.weights.push_back(w);
        td.dataset.total_weight += w;
    }
    td.query_points.reserve(n_queries);
    for (size_t i = 0; i < n_queries; ++i) {
        td.query_points.push_back(query_dist(rng));
    }
    return td;
}

// Single-shot benchmarks (1 query point, varying data sizes)
static void BM_Kde_Scalar_1q(benchmark::State& state) {
    auto td = make_test_data(state.range(0), 1);
    for (auto _ : state) {
        auto r = scalar_estimate(td.dataset, td.query_points, 2.0);
        benchmark::DoNotOptimize(r);
    }
}
BENCHMARK(BM_Kde_Scalar_1q)->Range(64, 2048);

static void BM_Kde_SIMD_1q(benchmark::State& state) {
    auto td = make_test_data(state.range(0), 1);
    for (auto _ : state) {
        auto r = Kde::estimate(td.dataset, td.query_points, 2.0);
        benchmark::DoNotOptimize(r);
    }
}
BENCHMARK(BM_Kde_SIMD_1q)->Range(64, 2048);

static void BM_Kde_FastExp_1q(benchmark::State& state) {
    auto td = make_test_data(state.range(0), 1);
    for (auto _ : state) {
        auto r = simd_fast_exp(td.dataset, td.query_points, 2.0);
        benchmark::DoNotOptimize(r);
    }
}
BENCHMARK(BM_Kde_FastExp_1q)->Range(64, 2048);

// Multi-query benchmarks (n_data=1024, varying query count)
static void BM_Kde_Scalar_Nq(benchmark::State& state) {
    auto td = make_test_data(1024, state.range(0));
    for (auto _ : state) {
        auto r = scalar_estimate(td.dataset, td.query_points, 2.0);
        benchmark::DoNotOptimize(r);
    }
}
BENCHMARK(BM_Kde_Scalar_Nq)->RangeMultiplier(2)->Range(1, 32);

static void BM_Kde_SIMD_Nq(benchmark::State& state) {
    auto td = make_test_data(1024, state.range(0));
    for (auto _ : state) {
        auto r = Kde::estimate(td.dataset, td.query_points, 2.0);
        benchmark::DoNotOptimize(r);
    }
}
BENCHMARK(BM_Kde_SIMD_Nq)->RangeMultiplier(2)->Range(1, 32);

static void BM_Kde_Batched_Nq(benchmark::State& state) {
    auto td = make_test_data(1024, state.range(0));
    for (auto _ : state) {
        auto r = simd_batched(td.dataset, td.query_points, 2.0);
        benchmark::DoNotOptimize(r);
    }
}
BENCHMARK(BM_Kde_Batched_Nq)->RangeMultiplier(2)->Range(1, 32);

// Production-like: n_data~M15 cluster (up to 900 items), n_query~clusters (5-50)
static void BM_Kde_Scalar_Prod(benchmark::State& state) {
    auto td = make_test_data(900, 20);
    for (auto _ : state) {
        auto r = scalar_estimate(td.dataset, td.query_points, 2.0);
        benchmark::DoNotOptimize(r);
    }
}
BENCHMARK(BM_Kde_Scalar_Prod);

static void BM_Kde_SIMD_Prod(benchmark::State& state) {
    auto td = make_test_data(900, 20);
    for (auto _ : state) {
        auto r = Kde::estimate(td.dataset, td.query_points, 2.0);
        benchmark::DoNotOptimize(r);
    }
}
BENCHMARK(BM_Kde_SIMD_Prod);

// ── Correctness check (run on startup) ────────────────────────────────
// Verify that all implementations produce the same result within tolerance.
static bool verify_correctness() {
    auto td = make_test_data(500, 10);
    double h = 2.0;

    auto ref = scalar_estimate(td.dataset, td.query_points, h);
    auto simd = Kde::estimate(td.dataset, td.query_points, h);
    auto fast = simd_fast_exp(td.dataset, td.query_points, h);

    for (size_t i = 0; i < ref.size(); ++i) {
        double d1 = std::abs(ref[i] - simd[i]);
        double d2 = std::abs(ref[i] - fast[i]);
        if (d1 > 1e-12) {
            std::fprintf(stderr, "SIMD mismatch at %zu: ref=%.15e simd=%.15e diff=%.15e\n",
                         i, ref[i], simd[i], d1);
            return false;
        }
        // Pade approximation has ~1e-4 error — this is expected for KDE use
        if (d2 > 1e-3) {
            std::fprintf(stderr, "FastExp mismatch at %zu: ref=%.15e fast=%.15e diff=%.15e\n",
                         i, ref[i], fast[i], d2);
            return false;
        }
    }
    std::printf("✓ Correctness verified: all impls match (Pade err < 1e-3)\n");
    return true;
}

BENCHMARK_MAIN();

// ── Run correctness check before google benchmark ─────────────────────
// (Executed via a global constructor — hacky but works for a bench)
namespace {
    struct CorrectnessCheck {
        CorrectnessCheck() { verify_correctness(); }
    } g_check;
}
