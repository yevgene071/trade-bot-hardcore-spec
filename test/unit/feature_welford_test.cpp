#include "features/FeatureExtractor.hpp"
#include "logger/Logger.hpp"
#include "marketdata/OrderBook.hpp"
#include "marketdata/TradeStream.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cmath>
#include <random>

using namespace trade_bot;

namespace {

class FeatureWelfordTest : public ::testing::Test {
protected:
    void SetUp() override { Logger::init(); }
};

// Apply an OrderBook snapshot whose best_bid+best_ask straddle `mid` symmetrically.
void push_mid(OrderBook& ob, double mid) {
    OrderBookSnapshot s{};
    s.ticker = "BTCUSDT";
    s.bids.push_back({mid - 0.005, 1.0, Side::Buy});
    s.asks.push_back({mid + 0.005, 1.0, Side::Sell});
    s.ts = std::chrono::system_clock::now();
    ob.apply_snapshot(s);
}

}  // namespace

// Spec AC: volatility_1min_bps within 0.5% of reference numpy.std on synthetic.
// Generate log-return series with known stdev σ_ref, drive the mid through
// the OrderBook, sample at 100 ms cadence, and compare.
TEST_F(FeatureWelfordTest, Volatility1minWithinHalfPercentOfReference) {
    OrderBook ob{"BTCUSDT", 1e-8, 1e-6};   // tiny tick → quantization loss negligible
    TradeStream ts{"BTCUSDT"};
    FeatureExtractor::Config cfg{};
    cfg.reserve_history = 700;     // 70 s @ 100 ms
    FeatureExtractor fe{"BTCUSDT", cfg};
    fe.set_sources(&ob, &ts, nullptr);

    constexpr double sigma     = 5e-5;   // ≈0.5 bps per tick
    constexpr int    n_steps   = 600;    // 60 s @ 100 ms
    constexpr double dt_ms     = 100.0;
    std::mt19937_64 rng{2026};
    std::normal_distribution<double> noise(0.0, sigma);

    auto now = std::chrono::system_clock::now();
    double mid  = 100.000;

    // Drive the extractor with the synthetic series and accumulate the
    // ground-truth Welford stats from the same returns at the same time.
    // Capture the *last* in-loop frame — adding an extra extract() after
    // the loop would push a duplicate-mid entry and bias volatility down.
    double ref_mean = 0.0;
    double ref_m2   = 0.0;
    int    ref_n    = 0;
    FeatureFrame last_frame{};
    for (int i = 0; i < n_steps; ++i) {
        const double r = noise(rng);
        mid *= std::exp(r);
        push_mid(ob, mid);
        last_frame = fe.extract(
            now + std::chrono::milliseconds{static_cast<int64_t>(i * dt_ms)});
        ++ref_n;
        const double delta = r - ref_mean;
        ref_mean += delta / ref_n;
        ref_m2   += delta * (r - ref_mean);
    }
    const double ref_stdev = std::sqrt(ref_m2 / (ref_n - 1));

    const double rel_err = std::abs(last_frame.volatility_1min - ref_stdev) / ref_stdev;
    EXPECT_LT(rel_err, 0.005)
        << "frame.volatility_1min=" << last_frame.volatility_1min
        << " ref=" << ref_stdev
        << " rel_err=" << rel_err;
}
