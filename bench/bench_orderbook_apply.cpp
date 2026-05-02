#include <benchmark/benchmark.h>
#include "marketdata/OrderBook.hpp"
#include <random>
#include <vector>

using namespace trade_bot;

static void BM_OrderBookApplyUpdate(benchmark::State& state) {
    OrderBook ob{"BTCUSDT", 0.01, 1e-8};
    
    std::mt19937_64 rng{42};
    std::uniform_real_distribution<double> price_dist(99.0, 101.0);
    std::uniform_real_distribution<double> size_dist(0.1, 10.0);
    std::uniform_int_distribution<int> side_dist(0, 1);

    auto make_upd = [&](double p, double sz, Side s) {
        OrderBookUpdate upd;
        upd.ticker = "BTCUSDT";
        upd.changes = {{p, sz, s}};
        upd.ts = std::chrono::system_clock::now();
        return upd;
    };

    // Warmup
    for (int i = 0; i < 1000; ++i) {
        double p = std::round(price_dist(rng) * 100.0) / 100.0;
        Side s = side_dist(rng) == 0 ? Side::Buy : Side::Sell;
        ob.apply_update(make_upd(p, size_dist(rng), s));
    }

    for (auto _ : state) {
        state.PauseTiming();
        double p = std::round(price_dist(rng) * 100.0) / 100.0;
        Side s = side_dist(rng) == 0 ? Side::Buy : Side::Sell;
        OrderBookUpdate upd = make_upd(p, size_dist(rng), s);
        state.ResumeTiming();
        
        ob.apply_update(upd);
    }
}
BENCHMARK(BM_OrderBookApplyUpdate);

BENCHMARK_MAIN();
