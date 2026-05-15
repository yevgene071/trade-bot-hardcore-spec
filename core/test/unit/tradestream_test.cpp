#include "marketdata/TradeStream.hpp"
#include <gtest/gtest.h>
#include <thread>
#include <random>

using namespace trade_bot;

class TradeStreamTest : public ::testing::Test {
protected:
    TradeStream ts{"BTCUSDT", 1.0, 0.5}; // alpha=1.0, beta=0.5
};

TEST_F(TradeStreamTest, WelfordStatsAreCorrect) {
    std::vector<double> sizes = {1.0, 2.0, 3.0, 4.0, 5.0};
    for (double s : sizes) {
        ts.on_trade({100.0, s, Side::Buy, std::chrono::system_clock::now()});
    }
    
    auto stats = ts.get_stats();
    EXPECT_NEAR(stats.avg_size, 3.0, 1e-7);
    // Population variance of {1,2,3,4,5} is 2.0, not 2.5 (sample).
    // implementation uses m2_ / n_
    EXPECT_NEAR(stats.stdev_size, std::sqrt(2.0), 1e-7);
}

TEST_F(TradeStreamTest, HawkesIntensityDecays) {
    auto now = std::chrono::system_clock::now();
    
    // 10 trades in a burst
    for (int i = 0; i < 10; ++i) {
        ts.on_trade({100.0, 1.0, Side::Buy, now});
    }
    
    ts.update(now);
    EXPECT_NEAR(ts.get_stats().hawkes_intensity_total, 10.0, 1e-7);
    
    // Decay for 2 seconds (beta=0.5)
    auto later = now + std::chrono::seconds(2);
    ts.update(later);
    
    // 10 * exp(-0.5 * 2) = 10 * exp(-1) approx 3.678
    EXPECT_NEAR(ts.get_stats().hawkes_intensity_total, 10.0 * std::exp(-1.0), 1e-7);
}

TEST_F(TradeStreamTest, VolumesAreCorrectByWindow) {
    auto now = std::chrono::system_clock::now();

    // Add in chronological order (oldest first) — ring buffer eviction requires sorted order
    // Trade 10s ago (4.0 size Buy)
    ts.on_trade({100.0, 4.0, Side::Buy, now - std::chrono::seconds(10)});
    // Trade 3s ago (2.0 size Sell)
    ts.on_trade({100.0, 2.0, Side::Sell, now - std::chrono::seconds(3)});
    // Trade 0.5s ago (1.0 size Buy)
    ts.on_trade({100.0, 1.0, Side::Buy, now - std::chrono::milliseconds(500)});
    
    ts.update(now);
    auto s = ts.get_stats();
    
    EXPECT_DOUBLE_EQ(s.buy_vol_1s, 1.0);
    EXPECT_DOUBLE_EQ(s.buy_vol_5s, 1.0);
    EXPECT_DOUBLE_EQ(s.buy_vol_30s, 5.0); // 1.0 + 4.0
    
    EXPECT_DOUBLE_EQ(s.sell_vol_1s, 0.0);
    EXPECT_DOUBLE_EQ(s.sell_vol_5s, 2.0);
    EXPECT_DOUBLE_EQ(s.sell_vol_30s, 2.0);
}

TEST_F(TradeStreamTest, TDigestQuantile) {
    std::mt19937_64 rng{42};
    std::exponential_distribution<double> dist(1.0); // mean=1.0, q99 approx 4.6
    
    for (int i = 0; i < 1000; ++i) {
        ts.on_trade({100.0, dist(rng), Side::Buy, std::chrono::system_clock::now()});
    }
    
    auto s = ts.get_stats();
    // exp(1.0) 99th percentile is -ln(1-0.99) = 4.605
    EXPECT_NEAR(s.q99_size, 4.605, 0.5); 
}
