#include <benchmark/benchmark.h>
#include "features/FeatureExtractor.hpp"
#include "marketdata/OrderBook.hpp"
#include "marketdata/TradeStream.hpp"
#include <random>
#include <chrono>

using namespace trade_bot;

static void BM_FeatureExtractorExtract(benchmark::State& state) {
    OrderBook       ob{"BTCUSDT", 0.01, 1e-8};
    TradeStream     ts{"BTCUSDT"};
    FeatureExtractor extractor{"BTCUSDT"};
    extractor.set_sources(&ob, &ts);

    std::mt19937_64 rng{42};
    std::uniform_real_distribution<double> price_dist(99900.0, 100100.0);
    std::uniform_real_distribution<double> size_dist(0.01, 5.0);
    std::uniform_int_distribution<int>     side_dist(0, 1);

    // Pre-fill 128 book levels (64 bid + 64 ask)
    for (int i = 0; i < 64; ++i) {
        double bid = std::round((100000.0 - i * 0.01) * 100.0) / 100.0;
        double ask = std::round((100000.0 + (i + 1) * 0.01) * 100.0) / 100.0;
        OrderBookUpdate upd;
        upd.ticker  = "BTCUSDT";
        upd.changes = {{bid, size_dist(rng), Side::Buy},
                       {ask, size_dist(rng), Side::Sell}};
        upd.ts = std::chrono::system_clock::now();
        ob.apply_update(upd);
    }

    // Pre-fill 500 trades
    auto now = std::chrono::system_clock::now();
    for (int i = 0; i < 500; ++i) {
        Trade t{price_dist(rng), size_dist(rng),
                side_dist(rng) == 0 ? Side::Buy : Side::Sell, now};
        ts.on_trade(t);
    }

    for (auto _ : state) {
        benchmark::DoNotOptimize(extractor.extract(std::chrono::system_clock::now()));
    }
}
BENCHMARK(BM_FeatureExtractorExtract);

BENCHMARK_MAIN();
