#include "strategy/BounceFromDensity.hpp"
#include "signals/SignalBus.hpp"
#include "logger/Logger.hpp"
#include <gtest/gtest.h>

using namespace trade_bot;

class BounceStrategyTest : public ::testing::Test {
protected:
    void SetUp() override { Logger::init(); }
};

TEST_F(BounceStrategyTest, GeneratesLongPlanFromBidDensity) {
    SignalBus bus;
    BounceFromDensity strategy("BTCUSDT");
    
    auto now = std::chrono::system_clock::now();
    double price = 50000.0;
    
    // Signals for bounce: Approach -> Density -> Fade
    Signal s1{SignalKind::LevelApproach, now, "BTCUSDT", price, 1.0, {}};
    Signal s2{SignalKind::DensityDetected, now, "BTCUSDT", price, 1.0, {{"side", "Bid"}}};
    Signal s3{SignalKind::TapeFade, now, "BTCUSDT", price, 1.0, {}};
    
    strategy.on_signal(s1);
    strategy.on_signal(s2);
    strategy.on_signal(s3);
    
    auto plan = strategy.tick(now);
    
    ASSERT_TRUE(plan.has_value());
    EXPECT_EQ(plan->side, Side::Buy);
    EXPECT_GT(plan->entry_price, price); // Long entry slightly above bid wall
    EXPECT_LT(plan->stop_price, price);  // Stop behind bid wall
    EXPECT_GT(plan->tp1_price, plan->entry_price);
}

TEST_F(BounceStrategyTest, InvalidatesWhenIcebergAppears) {
    SignalBus bus;
    BounceFromDensity strategy("BTCUSDT");
    auto now = std::chrono::system_clock::now();
    double price = 50000.0;
    
    strategy.on_signal({SignalKind::LevelApproach, now, "BTCUSDT", price, 1.0, {}});
    strategy.on_signal({SignalKind::DensityDetected, now, "BTCUSDT", price, 1.0, {{"side", "Bid"}}});
    strategy.on_signal({SignalKind::TapeFade, now, "BTCUSDT", price, 1.0, {}});
    
    // Before tick, an iceberg appears on the same level
    strategy.on_signal({SignalKind::IcebergSuspected, now, "BTCUSDT", price, 1.0, {}});
    
    auto plan = strategy.tick(now);
    EXPECT_FALSE(plan.has_value());
}
