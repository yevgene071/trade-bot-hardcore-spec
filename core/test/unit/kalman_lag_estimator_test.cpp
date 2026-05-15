#include "marketdata/KalmanLagEstimator.hpp"
#include "logger/Logger.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <random>
#include <vector>

using namespace trade_bot;

namespace {

class KalmanTest : public ::testing::Test {
protected:
    void SetUp() override { Logger::init(); }
};

}  // namespace

TEST_F(KalmanTest, TracksSlowLagDrift200To250Within20ms) {
    KalmanLagEstimator::Config cfg{};
    cfg.initial_lag_ms = 200.0;
    cfg.r_obs = 100.0;     // σ=10 ms observations
    cfg.q_lag = 0.5;
    cfg.q_drift = 0.005;
    KalmanLagEstimator k{cfg};

    constexpr int    steps = 100;     // 10 s @ 100 ms
    constexpr double dt    = 0.1;
    std::mt19937_64 rng{42};
    std::normal_distribution<double> noise(0.0, 10.0);

    for (int i = 0; i < steps; ++i) {
        const double truth = 200.0 + 50.0 * (static_cast<double>(i) / steps);
        const double obs   = truth + noise(rng);
        k.update(dt, obs);
    }

    EXPECT_NEAR(k.lag_ms(), 250.0, 20.0)
        << "drift tracking error too large: lag=" << k.lag_ms();
    EXPECT_GT(k.confidence(), 0.5);
}

TEST_F(KalmanTest, NoisyObservationsStdevBelow30ms) {
    KalmanLagEstimator::Config cfg{};
    cfg.initial_lag_ms = 200.0;
    cfg.r_obs = 2500.0;   // σ=50 ms — matches spec
    cfg.q_lag = 1.0;
    cfg.q_drift = 0.01;
    KalmanLagEstimator k{cfg};

    constexpr int    steps = 400;     // 40 s burn-in
    constexpr double dt    = 0.1;
    std::mt19937_64 rng{1};
    std::normal_distribution<double> noise(0.0, 50.0);

    std::vector<double> tail_estimates;
    for (int i = 0; i < steps; ++i) {
        k.update(dt, 200.0 + noise(rng));
        if (i > steps - 100) tail_estimates.push_back(k.lag_ms());
    }

    double mean = 0;
    for (auto v : tail_estimates) mean += v;
    mean /= tail_estimates.size();
    double sq = 0;
    for (auto v : tail_estimates) sq += (v - mean) * (v - mean);
    const double stdev = std::sqrt(sq / tail_estimates.size());

    EXPECT_LT(stdev, 30.0) << "observed Kalman output stdev=" << stdev;
}

TEST_F(KalmanTest, ResetReturnsToInitial) {
    KalmanLagEstimator k{};
    for (int i = 0; i < 50; ++i) k.update(0.1, 500.0);
    EXPECT_GT(std::abs(k.lag_ms()), 100.0);
    k.reset();
    EXPECT_NEAR(k.lag_ms(), 0.0, 1e-9);
}
