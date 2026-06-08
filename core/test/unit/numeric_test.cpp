#include <gtest/gtest.h>
#include "numeric/FixedPoint.hpp"
#include "numeric/Welford.hpp"
#include "numeric/WelfordCorrelation.hpp"
#include "numeric/Kahan.hpp"
#include "numeric/Ema.hpp"
#include "numeric/HdrHistogramWrapper.hpp"
#include <vector>

using namespace trade_bot;

TEST(NumericTest, FixedPointPriceRoundTrip) {
    double price = 65432.1;
    double tick_size = 0.1;
    PriceTick pt = PriceTick::from_price(price, tick_size);
    EXPECT_EQ(pt.ticks, 654321);
    EXPECT_DOUBLE_EQ(pt.to_price(tick_size), 65432.1);
}

TEST(NumericTest, FixedPointComparison) {
    PriceTick p1{100};
    PriceTick p2{200};
    EXPECT_TRUE(p1 < p2);
    EXPECT_TRUE(p2 > p1);
    EXPECT_EQ(p1 + PriceTick{100}, p2);
}

TEST(NumericTest, WelfordStats) {
    WelfordAccumulator<double> acc;
    std::vector<double> data = {10.0, 20.0, 30.0};
    for (double x : data) acc.update(x);
    
    EXPECT_EQ(acc.count(), 3);
    EXPECT_DOUBLE_EQ(acc.mean(), 20.0);
    EXPECT_DOUBLE_EQ(acc.variance(), 100.0); // sample variance: 200 / (3 - 1)
    EXPECT_NEAR(acc.stdev(), 10.0, 0.0001);
}

TEST(NumericTest, KahanSummation) {
    KahanAccumulator<double> acc;
    for (int i = 0; i < 10000; ++i) {
        acc.add(0.1);
    }
    // Naive sum of 0.1 ten thousand times would accumulate significant error
    EXPECT_DOUBLE_EQ(acc.sum(), 1000.0);
}

TEST(NumericTest, EmaReaction) {
    Ema<double> ema = Ema<double>::from_period(9); // alpha = 2/10 = 0.2
    ema.update(100.0);
    EXPECT_DOUBLE_EQ(ema.value(), 100.0);
    ema.update(110.0);
    // 0.2 * 110 + 0.8 * 100 = 22 + 80 = 102
    EXPECT_DOUBLE_EQ(ema.value(), 102.0);
}

TEST(NumericTest, PearsonCorrelation) {
    WelfordCorrelation<double> corr;
    // Perfect correlation: Y = 2X + 5
    for (int i = 1; i <= 10; ++i) {
        corr.update(static_cast<double>(i), static_cast<double>(2 * i + 5));
    }
    EXPECT_NEAR(corr.correlation(), 1.0, 0.000001);

    WelfordCorrelation<double> corr2;
    // Perfect inverse correlation: Y = -X
    for (int i = 1; i <= 10; ++i) {
        corr2.update(static_cast<double>(i), static_cast<double>(-i));
    }
    EXPECT_NEAR(corr2.correlation(), -1.0, 0.000001);
}

TEST(NumericTest, HdrHistogramBasic) {
    HdrHistogram hist(1000000, 3);
    hist.record(100);
    hist.record(200);
    hist.record(300);
    
    EXPECT_EQ(hist.min(), 100);
    EXPECT_EQ(hist.max(), 300);
    EXPECT_NEAR(hist.mean(), 200.0, 0.01);
    EXPECT_GE(hist.value_at_percentile(50.0), 200);
}
