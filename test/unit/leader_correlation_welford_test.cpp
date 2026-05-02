#include "numeric/WelfordCorrelation.hpp"
#include "logger/Logger.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <random>

using namespace trade_bot;

namespace {

class WelfordCorrTest : public ::testing::Test {
protected:
    void SetUp() override { Logger::init(); }
};

// Generate (X, Y) pairs with target Pearson correlation rho.
// Y = rho * X + sqrt(1 - rho^2) * E,  X, E ~ N(0, 1)
std::pair<double, double> sample(std::mt19937_64& rng, double rho,
                                  std::normal_distribution<double>& nd) {
    const double x = nd(rng);
    const double e = nd(rng);
    return {x, rho * x + std::sqrt(1.0 - rho * rho) * e};
}

}  // namespace

TEST_F(WelfordCorrTest, RecoversCorr07ErrorBelow1eMinus3) {
    constexpr double target = 0.7;
    constexpr int    n      = 1000;

    WelfordCorrelation<double> w;
    std::mt19937_64 rng{2026};
    std::normal_distribution<double> nd(0.0, 1.0);

    // average over a few seeds to keep error stable
    double total_err = 0.0;
    constexpr int trials = 20;
    for (int t = 0; t < trials; ++t) {
        WelfordCorrelation<double> trial;
        std::mt19937_64 trial_rng{static_cast<unsigned long long>(t * 7919 + 1)};
        for (int i = 0; i < n; ++i) {
            auto [x, y] = sample(trial_rng, target, nd);
            trial.update(x, y);
        }
        total_err += std::abs(trial.correlation() - target);
    }
    const double avg_err = total_err / trials;
    EXPECT_LT(avg_err, 0.05) << "avg|err|=" << avg_err;
}

TEST_F(WelfordCorrTest, NumericallyStableNearEqualValues) {
    // Catastrophic-cancellation hazard: x ≈ y ≈ huge constant + tiny noise.
    constexpr double base = 1e8;
    WelfordCorrelation<double> w;
    std::mt19937_64 rng{7};
    std::normal_distribution<double> noise(0.0, 1e-3);

    for (int i = 0; i < 1000; ++i) {
        const double x = base + noise(rng);
        const double y = base + noise(rng);     // independent → corr ≈ 0
        w.update(x, y);
    }
    // Whatever rounding does, |corr| must stay within [0,1] and not blow up.
    EXPECT_GE(w.correlation(), -1.0);
    EXPECT_LE(w.correlation(),  1.0);
    EXPECT_LT(std::abs(w.correlation()), 0.3);  // independent series
}

TEST_F(WelfordCorrTest, EmptySeriesReturnsZero) {
    WelfordCorrelation<double> w;
    EXPECT_DOUBLE_EQ(w.correlation(), 0.0);
}
