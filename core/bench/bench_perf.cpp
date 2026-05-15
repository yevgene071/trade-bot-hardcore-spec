#include <benchmark/benchmark.h>
#include "perf/SimdOps.hpp"
#include <vector>
#include <numeric>

using namespace trade_bot;

static void BM_SimdSum(benchmark::State& state) {
    std::vector<double> data(state.range(0), 1.1);
    for (auto _ : state) {
        benchmark::DoNotOptimize(SimdOps::sum(data.data(), data.size()));
    }
}
BENCHMARK(BM_SimdSum)->Range(8, 8192);

static void BM_ScalarSum(benchmark::State& state) {
    std::vector<double> data(state.range(0), 1.1);
    for (auto _ : state) {
        benchmark::DoNotOptimize(std::accumulate(data.begin(), data.end(), 0.0));
    }
}
BENCHMARK(BM_ScalarSum)->Range(8, 8192);

BENCHMARK_MAIN();
