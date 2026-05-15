#include <benchmark/benchmark.h>
#include "perf/SimdOps.hpp"
#include <vector>
#include <numeric>

using namespace trade_bot;

static double scalar_sum(const double* data, size_t n) {
    double total = 0.0;
    for (size_t i = 0; i < n; ++i) {
        total += data[i];
    }
    return total;
}

static void BM_SimdSum_10(benchmark::State& state) {
    std::vector<double> data(10, 1.23);
    for (auto _ : state) {
        benchmark::DoNotOptimize(SimdOps::sum(data.data(), data.size()));
    }
}
BENCHMARK(BM_SimdSum_10);

static void BM_ScalarSum_10(benchmark::State& state) {
    std::vector<double> data(10, 1.23);
    for (auto _ : state) {
        benchmark::DoNotOptimize(scalar_sum(data.data(), data.size()));
    }
}
BENCHMARK(BM_ScalarSum_10);

BENCHMARK_MAIN();
