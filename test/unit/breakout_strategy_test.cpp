#include "strategy/BreakoutEatThrough.hpp"
#include "signals/SignalBus.hpp"
#include "logger/Logger.hpp"
#include <gtest/gtest.h>

using namespace trade_bot;

class BreakoutStrategyTest : public ::testing::Test {
protected:
    void SetUp() override { Logger::init(); }
};

TEST_F(BreakoutStrategyTest, GeneratesLongPlanFromAskEating) {
    BreakoutEatThrough strategy("BTCUSDT");
    auto now = std::chrono::system_clock::now();
    double price = 50000.0;
    
    // Signals for breakout: Support behind -> Eating -> Burst
    Signal s_support{SignalKind::DensityDetected, now - std::chrono::seconds(10), "BTCUSDT", 49980.0, 1.0, {{"side", "Bid"}}};
    Signal s_eating{SignalKind::DensityEating, now, "BTCUSDT", price, 1.0, {{"side", "Ask"}}};
    Signal s_burst{SignalKind::TapeBurst, now, "BTCUSDT", price, 1.0, {{"side", "Buy"}}};
    
    strategy.on_signal(s_support);
    strategy.on_signal(s_eating);
    strategy.on_signal(s_burst);
    
    auto plan = strategy.tick(now);
    
    ASSERT_TRUE(plan.has_value());
    EXPECT_EQ(plan->side, Side::Buy);
    EXPECT_EQ(plan->entry_type, OrderType::Market);
    EXPECT_GT(plan->entry_price, price); 
    EXPECT_LT(plan->stop_price, 49980.0); // Stop behind support
}

TEST_F(BreakoutStrategyTest, NoPlanWithoutSupport) {
    BreakoutEatThrough strategy("BTCUSDT");
    auto now = std::chrono::system_clock::now();
    double price = 50000.0;
    
    strategy.on_signal({SignalKind::DensityEating, now, "BTCUSDT", price, 1.0, {{"side", "Ask"}}});
    strategy.on_signal({SignalKind::TapeBurst, now, "BTCUSDT", price, 1.0, {{"side", "Buy"}}});
    
    auto plan = strategy.tick(now);
    EXPECT_FALSE(plan.has_value());
}
