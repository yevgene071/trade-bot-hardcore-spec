#include "numeric/Kde.hpp"

#include <random>
#include <vector>
#include <cmath>
#include <cstdio>
#include <chrono>
#include <algorithm>
#include <xsimd/xsimd.hpp>

using namespace trade_bot;

// ── Inline to prevent compiler optimising away a return value ──────────
template <typename T>
static void escape(T&& t) {
    asm volatile("" : : "r,m"(t) : "memory");
}

// ── Scalar reference implementation ──────────────────────────────────────
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

// ── Optimised: Pade-approximated exp (no xsimd::exp call) ────────────────
static std::vector<double> simd_pade(const Kde::DataSet& data,
                                      const std::vector<double>& query_points,
                                      double bandwidth) {
    if (data.values.empty() || query_points.empty()) return {};
    if (bandwidth <= 0) bandwidth = 1e-6;
    std::vector<double> densities(query_points.size(), 0.0);
    double inv_h = 1.0 / bandwidth;
    if (data.total_weight <= 0) return densities;

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
            batch_type diff2 = diff * diff;
            // a = 0.5 * diff^2
            batch_type a = batch_type(0.5) * diff2;
            // clamp a to avoid extreme values
            a = xsimd::min(a, batch_type(12.0));
            // [2/2] Pade approximant for exp(-a): (1 - 7/30*a + a^2/40) / (1 + 4/15*a + a^2/24)
            batch_type a2 = a * a;
            batch_type num = batch_type(1.0) + batch_type(-0.23333333333) * a + batch_type(0.025) * a2;
            batch_type den = batch_type(1.0) + batch_type(0.26666666667) * a + batch_type(0.04166666667) * a2;
            batch_type kernel_val = num / den;
            acc += w_batch * kernel_val;
        }

        double sum = xsimd::reduce_add(acc);
        for (; j < data.values.size(); ++j) {
            double diff = (q - data.values[j]) * inv_h;
            double a = 0.5 * diff * diff;
            if (a < 12.0) {
                double a2 = a * a;
                double num = 1.0 + (-0.23333333333) * a + 0.025 * a2;
                double den = 1.0 + 0.26666666667 * a + 0.04166666667 * a2;
                sum += data.weights[j] * (num / den);
            }
        }

        densities[i] = sum / (data.total_weight * bandwidth * kSqrt2Pi);
    }
    return densities;
}

// ── Optimised: Loop-unrolled SIMD (process 2×SIMD batches) ─────────────
static std::vector<double> simd_unrolled2x(const Kde::DataSet& data,
                                            const std::vector<double>& query_points,
                                            double bandwidth) {
    if (data.values.empty() || query_points.empty()) return {};
    if (bandwidth <= 0) bandwidth = 1e-6;
    std::vector<double> densities(query_points.size(), 0.0);
    double inv_h = 1.0 / bandwidth;
    if (data.total_weight <= 0) return densities;

    using batch_type = xsimd::batch<double>;
    constexpr size_t simd_size = batch_type::size;
    constexpr size_t unroll = 2;

    for (size_t i = 0; i < query_points.size(); ++i) {
        double q = query_points[i];
        size_t j = 0;
        batch_type q_batch(q);
        batch_type inv_h_batch(inv_h);
        batch_type acc0(0.0), acc1(0.0);

        for (; j + unroll * simd_size <= data.values.size(); j += unroll * simd_size) {
            batch_type x0 = batch_type::load_unaligned(&data.values[j]);
            batch_type w0 = batch_type::load_unaligned(&data.weights[j]);
            batch_type diff0 = (q_batch - x0) * inv_h_batch;
            acc0 += w0 * xsimd::exp(batch_type(-0.5) * diff0 * diff0);

            batch_type x1 = batch_type::load_unaligned(&data.values[j + simd_size]);
            batch_type w1 = batch_type::load_unaligned(&data.weights[j + simd_size]);
            batch_type diff1 = (q_batch - x1) * inv_h_batch;
            acc1 += w1 * xsimd::exp(batch_type(-0.5) * diff1 * diff1);
        }

        double sum = xsimd::reduce_add(acc0) + xsimd::reduce_add(acc1);

        for (; j + simd_size <= data.values.size(); j += simd_size) {
            batch_type x_batch = batch_type::load_unaligned(&data.values[j]);
            batch_type w_batch = batch_type::load_unaligned(&data.weights[j]);
            batch_type diff = (q_batch - x_batch) * inv_h_batch;
            batch_type kv = w_batch * xsimd::exp(batch_type(-0.5) * diff * diff);
            sum += xsimd::reduce_add(kv);
        }

        for (; j < data.values.size(); ++j) {
            double diff = (q - data.values[j]) * inv_h;
            sum += data.weights[j] * std::exp(-0.5 * diff * diff);
        }

        densities[i] = sum / (data.total_weight * bandwidth * kSqrt2Pi);
    }
    return densities;
}

// ── Hybrid: SIMD+Pade, unrolled 2×, with prefetch ──────────────────────
static std::vector<double> simd_hybrid(const Kde::DataSet& data,
                                        const std::vector<double>& query_points,
                                        double bandwidth) {
    if (data.values.empty() || query_points.empty()) return {};
    if (bandwidth <= 0) bandwidth = 1e-6;
    std::vector<double> densities(query_points.size(), 0.0);
    double inv_h = 1.0 / bandwidth;
    if (data.total_weight <= 0) return densities;

    using batch_type = xsimd::batch<double>;
    constexpr size_t simd_size = batch_type::size;
    constexpr size_t unroll = 2;

    for (size_t i = 0; i < query_points.size(); ++i) {
        double q = query_points[i];
        size_t j = 0;
        batch_type q_batch(q);
        batch_type inv_h_batch(inv_h);
        batch_type acc0(0.0), acc1(0.0);

        constexpr double kNeg7_30  = -0.23333333333;
        constexpr double k1_40     = 0.025;
        constexpr double k4_15     = 0.26666666667;
        constexpr double k1_24     = 0.04166666667;

        for (; j + unroll * simd_size <= data.values.size(); j += unroll * simd_size) {
            __builtin_prefetch(&data.values[j + 64], 0, 0);
            __builtin_prefetch(&data.weights[j + 64], 0, 0);

            batch_type x0 = batch_type::load_unaligned(&data.values[j]);
            batch_type w0 = batch_type::load_unaligned(&data.weights[j]);
            batch_type diff0 = (q_batch - x0) * inv_h_batch;
            batch_type a0 = batch_type(0.5) * diff0 * diff0;
            a0 = xsimd::min(a0, batch_type(12.0));
            batch_type a20 = a0 * a0;
            batch_type num0 = batch_type(1.0) + batch_type(kNeg7_30) * a0 + batch_type(k1_40) * a20;
            batch_type den0 = batch_type(1.0) + batch_type(k4_15) * a0 + batch_type(k1_24) * a20;
            acc0 += w0 * (num0 / den0);

            batch_type x1 = batch_type::load_unaligned(&data.values[j + simd_size]);
            batch_type w1 = batch_type::load_unaligned(&data.weights[j + simd_size]);
            batch_type diff1 = (q_batch - x1) * inv_h_batch;
            batch_type a1 = batch_type(0.5) * diff1 * diff1;
            a1 = xsimd::min(a1, batch_type(12.0));
            batch_type a21 = a1 * a1;
            batch_type num1 = batch_type(1.0) + batch_type(kNeg7_30) * a1 + batch_type(k1_40) * a21;
            batch_type den1 = batch_type(1.0) + batch_type(k4_15) * a1 + batch_type(k1_24) * a21;
            acc1 += w1 * (num1 / den1);
        }

        double sum = xsimd::reduce_add(acc0) + xsimd::reduce_add(acc1);

        for (; j + simd_size <= data.values.size(); j += simd_size) {
            batch_type x_batch = batch_type::load_unaligned(&data.values[j]);
            batch_type w_batch = batch_type::load_unaligned(&data.weights[j]);
            batch_type diff = (q_batch - x_batch) * inv_h_batch;
            batch_type a = batch_type(0.5) * diff * diff;
            a = xsimd::min(a, batch_type(12.0));
            batch_type a2 = a * a;
            batch_type num = batch_type(1.0) + batch_type(kNeg7_30) * a + batch_type(k1_40) * a2;
            batch_type den = batch_type(1.0) + batch_type(k4_15) * a + batch_type(k1_24) * a2;
            batch_type kv = w_batch * (num / den);
            sum += xsimd::reduce_add(kv);
        }

        for (; j < data.values.size(); ++j) {
            double diff = (q - data.values[j]) * inv_h;
            double a = 0.5 * diff * diff;
            if (a < 12.0) {
                double a2 = a * a;
                double num = 1.0 + kNeg7_30 * a + k1_40 * a2;
                double den = 1.0 + k4_15 * a + k1_24 * a2;
                sum += data.weights[j] * (num / den);
            }
        }

        densities[i] = sum / (data.total_weight * bandwidth * kSqrt2Pi);
    }
    return densities;
}

// ── Test data ────────────────────────────────────────────────────────────
struct TestData {
    Kde::DataSet dataset;
    std::vector<double> query_points;
};

static TestData make_data(size_t n_data, size_t n_queries) {
    std::mt19937 rng(42);
    std::uniform_real_distribution<double> pd(100.0, 200.0);
    std::uniform_real_distribution<double> wd(0.5, 10.0);
    std::uniform_real_distribution<double> qd(100.0, 200.0);
    TestData td;
    td.dataset.values.reserve(n_data);
    td.dataset.weights.reserve(n_data);
    for (size_t i = 0; i < n_data; ++i) {
        td.dataset.values.push_back(pd(rng));
        double w = wd(rng);
        td.dataset.weights.push_back(w);
        td.dataset.total_weight += w;
    }
    td.query_points.reserve(n_queries);
    for (size_t i = 0; i < n_queries; ++i)
        td.query_points.push_back(qd(rng));
    return td;
}

// ── Timing ───────────────────────────────────────────────────────────────
using Clock = std::chrono::high_resolution_clock;

template <typename F>
static double time_us(F&& fn) {
    // Warmup
    for (int w = 0; w < 3; ++w) fn();
    // Timed
    const int reps = 20;
    auto start = Clock::now();
    for (int r = 0; r < reps; ++r) fn();
    auto end = Clock::now();
    return std::chrono::duration<double, std::micro>(end - start).count() / reps;
}

static void run_bench(const char* name, auto fn, const TestData& td, double h) {
    double us = time_us([&]() { auto r = fn(td.dataset, td.query_points, h); escape(r.data()); });
    std::printf("  %-25s  %8.1f µs/call\n", name, us);
}

// ── Correctness ─────────────────────────────────────────────────────────
static bool verify() {
    auto td = make_data(500, 20);
    double h = 2.0;
    auto ref  = scalar_estimate(td.dataset, td.query_points, h);
    auto simd = Kde::estimate(td.dataset, td.query_points, h);
    auto pade = simd_pade(td.dataset, td.query_points, h);
    auto unr  = simd_unrolled2x(td.dataset, td.query_points, h);
    auto hyb  = simd_hybrid(td.dataset, td.query_points, h);

    bool ok = true;
    for (size_t i = 0; i < ref.size(); ++i) {
        double d_simd = std::abs(ref[i] - simd[i]);
        double d_pade = std::abs(ref[i] - pade[i]);
        double d_unr  = std::abs(ref[i] - unr[i]);
        double d_hyb  = std::abs(ref[i] - hyb[i]);
        if (d_simd > 1e-14) { std::printf("✗ SIMD mismatch[%zu]: %.15e\n", i, d_simd); ok = false; }
        if (d_pade > 1e-3)  { std::printf("✗ PADE mismatch[%zu]: %.15e\n", i, d_pade); ok = false; }
        if (d_unr > 1e-14)  { std::printf("✗ UNROLL mismatch[%zu]: %.15e\n", i, d_unr); ok = false; }
        if (d_hyb > 1e-3)   { std::printf("✗ HYBRID mismatch[%zu]: %.15e\n", i, d_hyb); ok = false; }
    }
    if (ok) std::printf("✓ All implementations match (Pade err < 1e-3)\n");
    return ok;
}

// ── Main ─────────────────────────────────────────────────────────────────
int main() {
    verify();

    std::printf("\n%s\n", std::string(80, '=').c_str());
    std::printf("  KDE BENCHMARK — n_data = 900 (M15 cluster), n_queries = 20 (clusters)\n");
    std::printf("  GCC 15.2.0, -O3 -mavx2 -march=native, xsimd 13.0\n");
    std::printf("%s\n\n", std::string(80, '=').c_str());

    auto prod = make_data(900, 20);
    double h = Kde::silverman_bandwidth(prod.dataset) * 1.0;

    run_bench("Scalar (no SIMD)",   scalar_estimate,   prod, h);
    run_bench("SIMD (xsimd::exp)",  Kde::estimate,     prod, h);
    run_bench("SIMD+Pade (no exp)", simd_pade,          prod, h);
    run_bench("SIMD+Unroll 2×",       simd_unrolled2x,    prod, h);
    run_bench("SIMD+Unroll+Pade",   simd_hybrid,        prod, h);

    // ── Vary data size (1 query point, like production) ─────────────────
    std::printf("\n%s\n", std::string(80, '-').c_str());
    std::printf("  SCALING: n_queries=1 (typical production call), varying n_data\n");
    std::printf("%s\n", std::string(80, '-').c_str());
    std::printf("  %-10s %-12s %-12s %-12s %-12s %-12s  %-10s  %-10s\n",
                "n_data", "Scalar", "SIMD", "Pade", "Unroll", "Hybrid", "S/Spd↑", "S/Hyb↑");
    std::printf("  %s\n", std::string(80, '-').c_str());

    for (int nd : {64, 128, 256, 512, 900, 1024, 2048}) {
        auto td = make_data(nd, 1);
        double s  = time_us([&]() { auto r = scalar_estimate(td.dataset, td.query_points, h); escape(r.data()); });
        double si = time_us([&]() { auto r = Kde::estimate(td.dataset, td.query_points, h); escape(r.data()); });
        double pa = time_us([&]() { auto r = simd_pade(td.dataset, td.query_points, h); escape(r.data()); });
        double un = time_us([&]() { auto r = simd_unrolled2x(td.dataset, td.query_points, h); escape(r.data()); });
        double hy = time_us([&]() { auto r = simd_hybrid(td.dataset, td.query_points, h); escape(r.data()); });
        std::printf("  %-10d %-12.1f %-12.1f %-12.1f %-12.1f %-12.1f  %-10.2f  %-10.2f\n",
                    nd, s, si, pa, un, hy, s / si, s / hy);
    }

    // ── Summary ─────────────────────────────────────────────────────────
    std::printf("\n%s\n", std::string(80, '=').c_str());
    std::printf("  KEY RESULTS\n");
    std::printf("%s\n", std::string(80, '=').c_str());

    for (int nd : {512, 900}) {
        auto td = make_data(nd, 1);
        double s  = time_us([&]() { auto r = scalar_estimate(td.dataset, td.query_points, h); escape(r.data()); });
        double si = time_us([&]() { auto r = Kde::estimate(td.dataset, td.query_points, h); escape(r.data()); });
        double hy = time_us([&]() { auto r = simd_hybrid(td.dataset, td.query_points, h); escape(r.data()); });
        std::printf("\n  n_data=%d, nq=1:\n", nd);
        std::printf("    Scalar:       %8.1f µs  (1.00×)\n", s);
        std::printf("    SIMD (orig):  %8.1f µs  (%5.2f×)\n", si, s / si);
        std::printf("    Hybrid:       %8.1f µs  (%5.2f×)\n", hy, s / hy);
    }

    return 0;
}
