#include "marketdata/LeaderTracker.hpp"
#include "logger/Logger.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <random>
#include <vector>

using namespace trade_bot;

namespace {

class LeaderCusumTest : public ::testing::Test {
protected:
    void SetUp() override { Logger::init(); }
};

// Build a stream where the *instantaneous* correlation steps from ~0.8 to
// ~0.3. The naive detector waits for the running Pearson correlation to fall
// below 0.5; CUSUM should alarm earlier because it accumulates the deficit.
std::vector<std::pair<double, double>> make_step_change_stream(int n_high, int n_low,
                                                               unsigned seed) {
    std::mt19937_64 rng{seed};
    std::normal_distribution<double> normal(0.0, 1.0);
    std::vector<std::pair<double, double>> out;
    out.reserve(n_high + n_low);

    auto append = [&](int n, double rho) {
        for (int i = 0; i < n; ++i) {
            const double x = normal(rng);
            const double e = normal(rng);
            const double y = rho * x + std::sqrt(1.0 - rho * rho) * e;
            out.emplace_back(x, y);
        }
    };
    append(n_high, 0.8);
    append(n_low,  0.3);
    return out;
}

}  // namespace

TEST_F(LeaderCusumTest, CusumAlarmsBeforeNaiveCorrelationThreshold) {
    constexpr int n_high = 200;
    constexpr int n_low  = 800;            // longer low regime so the naive
                                           // running-Pearson eventually drops
                                           // below 0.5 (weighted dominance).
    constexpr int n_per_sec = 10;          // 100 ms cadence
    auto stream = make_step_change_stream(n_high, n_low, /*seed=*/123);

    // -- CUSUM-aware tracker --
    LeaderTracker::Config cfg{};
    cfg.cusum_baseline  = 0.7;
    cfg.cusum_drift     = 0.05;
    cfg.cusum_threshold = 1.0;
    LeaderTracker tracker{cfg};

    int cusum_idx = -1;
    int naive_idx = -1;

    constexpr std::size_t naive_warmup = 30;
    for (std::size_t i = 0; i < stream.size(); ++i) {
        tracker.update(stream[i].first, stream[i].second);
        if (cusum_idx < 0 && tracker.decorrelation_alarm()) {
            cusum_idx = static_cast<int>(i);
        }
        if (naive_idx < 0 && tracker.sample_count() > naive_warmup &&
            tracker.correlation() < 0.5) {
            naive_idx = static_cast<int>(i);
        }
        if (cusum_idx >= 0 && naive_idx >= 0) break;
    }

    ASSERT_GE(cusum_idx, 0) << "CUSUM never fired";
    ASSERT_GE(naive_idx, 0) << "Naive threshold never fired";
    const int delta_steps = naive_idx - cusum_idx;
    const double delta_ms = delta_steps * (1000.0 / n_per_sec);
    EXPECT_GE(delta_ms, 300.0)
        << "CUSUM should beat naive by at least 300 ms; observed " << delta_ms << " ms";
}
