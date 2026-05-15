#include "numeric/ZigZag.hpp"
#include <gtest/gtest.h>

using namespace trade_bot;

TEST(ZigZagTest, DetectsPullbacks) {
    // 100 -> 105 -> 95 -> 110 -> 90
    // Swings: 5, -10, 15, -20 (all >= 5 bps)
    std::vector<double> prices = {
        100.0, 101.0, 102.0, 103.0, 104.0, 105.0, // Up
        104.0, 103.0, 102.0, 101.0, 100.0, 95.0,  // Down (~100 bps reversal)
        96.0, 97.0, 98.0, 99.0, 100.0, 110.0,     // Up
        105.0, 100.0, 95.0, 90.0                  // Down
    };
    
    auto peaks = ZigZag::calculate(prices, 5.0); // 5 bps threshold
    
    // We expect peaks at 105.0, 95.0, 110.0
    // (Last move to 90.0 might not be a peak yet if it's the end)
    ASSERT_GE(peaks.size(), 3);
    EXPECT_DOUBLE_EQ(peaks[0].price, 105.0);
    EXPECT_TRUE(peaks[0].is_high);
    EXPECT_DOUBLE_EQ(peaks[1].price, 95.0);
    EXPECT_FALSE(peaks[1].is_high);
    EXPECT_DOUBLE_EQ(peaks[2].price, 110.0);
    EXPECT_TRUE(peaks[2].is_high);
}

TEST(ZigZagTest, IgnoresSmallNoise) {
    // 100 -> 100.05 -> 100 -> 100.05 ... (5 bps noise)
    std::vector<double> prices = {
        100.0, 100.05, 100.0, 100.05, 100.0, 100.05
    };
    auto peaks = ZigZag::calculate(prices, 10.0); // 10 bps threshold
    EXPECT_TRUE(peaks.empty());
}
