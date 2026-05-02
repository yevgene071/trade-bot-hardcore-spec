#include "marketdata/LeaderTracker.hpp"
#include "logger/Logger.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <random>
#include <vector>

using namespace trade_bot;

namespace {

class LeaderTrackerTest : public ::testing::Test {
protected:
    void SetUp() override { Logger::init(); }
};

}  // namespace

TEST_F(LeaderTrackerTest, AltMirrorsBtcWith200msShift) {
    constexpr int    n_leader_pre = 200;   // pre-roll
    constexpr int    n_total      = 2000;  // 200 s @ 100 ms
    constexpr int    shift_steps  = 2;     // 2 × 100 ms = 200 ms
    constexpr double price_scale  = 1.5;

    std::mt19937_64 rng{2026};
    std::normal_distribution<double> tick(0.0, 0.5);
    std::normal_distribution<double> noise(0.0, 0.05);  // small follower noise

    // Random walk for BTC.
    std::vector<double> btc(n_leader_pre + n_total, 60'000.0);
    for (std::size_t i = 1; i < btc.size(); ++i) {
        btc[i] = btc[i - 1] + tick(rng);
    }
    // Follower = scale * btc[i - shift] + noise
    LeaderTracker::Config cfg{};
    cfg.kalman.initial_lag_ms = 0.0;
    cfg.kalman.r_obs = 100.0;
    LeaderTracker tracker{cfg};

    for (int i = 0; i < n_total; ++i) {
        const double leader   = btc[i + n_leader_pre];
        const double follower = price_scale * btc[i + n_leader_pre - shift_steps] +
                                price_scale * noise(rng);
        tracker.update(leader, follower);
    }

    EXPECT_GT(tracker.correlation(), 0.95);
    EXPECT_GE(tracker.lag_ms(), 150.0);
    EXPECT_LE(tracker.lag_ms(), 250.0);
    EXPECT_GT(tracker.confidence(), 0.7);
}
