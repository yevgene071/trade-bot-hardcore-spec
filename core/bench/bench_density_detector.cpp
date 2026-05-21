#include <benchmark/benchmark.h>
#include "signals/DensityDetector.hpp"
#include "signals/SignalBus.hpp"
#include "marketdata/OrderBook.hpp"
#include "universe/TickerUniverse.hpp"
#include "features/FeatureFrame.hpp"
#include <random>
#include <chrono>

using namespace trade_bot;

static void BM_DensityDetectorOnBookUpdateAndFrame(benchmark::State& state) {
    OrderBook       ob{"BTCUSDT", 0.01, 1e-8};
    TickerUniverse  universe;
    SignalBus       bus;
    DensityDetector detector{"BTCUSDT", bus, ob, universe};

    std::mt19937_64 rng{42};
    std::uniform_real_distribution<double> price_dist(99900.0, 100100.0);
    std::uniform_real_distribution<double> size_dist(0.1, 50.0);
    std::uniform_int_distribution<int>     side_dist(0, 1);

    // Pre-fill book with 64 bid + 64 ask levels
    for (int i = 0; i < 64; ++i) {
        double bid = std::round((100000.0 - i * 0.01) * 100.0) / 100.0;
        double ask = std::round((100000.0 + (i + 1) * 0.01) * 100.0) / 100.0;
        OrderBookUpdate upd;
        upd.ticker  = "BTCUSDT";
        upd.changes = {{bid, size_dist(rng), Side::Buy},
                       {ask, size_dist(rng), Side::Sell}};
        upd.ts = std::chrono::system_clock::now();
        ob.apply_update(upd);
        detector.on_book_update(upd);
    }

    FeatureFrame frame{};
    frame.ticker    = "BTCUSDT";
    frame.mid       = 100000.0;
    frame.best_bid  =  99999.99;
    frame.best_ask  = 100000.01;
    frame.timestamp = std::chrono::system_clock::now();

    for (auto _ : state) {
        state.PauseTiming();
        double p = std::round(price_dist(rng) * 100.0) / 100.0;
        Side   s = side_dist(rng) == 0 ? Side::Buy : Side::Sell;
        OrderBookUpdate upd;
        upd.ticker  = "BTCUSDT";
        upd.changes = {{p, size_dist(rng), s}};
        upd.ts = std::chrono::system_clock::now();
        state.ResumeTiming();

        ob.apply_update(upd);
        detector.on_book_update(upd);
        detector.on_frame(frame);
    }
}
BENCHMARK(BM_DensityDetectorOnBookUpdateAndFrame);

BENCHMARK_MAIN();
