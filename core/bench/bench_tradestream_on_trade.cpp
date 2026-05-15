#include <benchmark/benchmark.h>
#include "marketdata/TradeStream.hpp"
#include <random>
#include <chrono>

using namespace trade_bot;

static void BM_TradeStreamOnTrade(benchmark::State& state) {
    TradeStream ts{"BTCUSDT"};
    auto now = std::chrono::system_clock::now();
    
    std::mt19937_64 rng{42};
    std::uniform_real_distribution<double> price_dist(99.0, 101.0);
    std::uniform_real_distribution<double> size_dist(0.1, 10.0);
    std::uniform_int_distribution<int> side_dist(0, 1);

    for (auto _ : state) {
        state.PauseTiming();
        double p = price_dist(rng);
        double sz = size_dist(rng);
        Side s = side_dist(rng) == 0 ? Side::Buy : Side::Sell;
        Trade t{p, sz, s, now};
        state.ResumeTiming();
        
        ts.on_trade(t);
    }
}
BENCHMARK(BM_TradeStreamOnTrade);

BENCHMARK_MAIN();
