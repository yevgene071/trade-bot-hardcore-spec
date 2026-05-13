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
    TickerInfo info{"BTCUSDT", "BTC", "USDT", true, 0.01, 1e-6, 0.0, 0.0};
    BreakoutEatThrough strategy("BTCUSDT", info);
    auto now = std::chrono::system_clock::now();
    double price = 50000.0;

    // Feed FeatureFrame with required fields
    FeatureFrame frame;
    frame.ticker = "BTCUSDT";
    frame.timestamp = now;
    frame.mid = price;
    frame.best_bid = price - 20.0;
    frame.best_ask = price - 15.0; // Ask is below density (already moved), distance = 3 bps > 2 bps threshold
    frame.buy_vol_5s = 200.0;     // Surge: 200 / (500/6) = 2.4 > 1.5 threshold
    frame.buy_vol_30s = 500.0;
    frame.sell_vol_5s = 50.0;
    frame.sell_vol_30s = 300.0;
    frame.tape_aggression = 0.4; // positive = buy pressure
    frame.leader_change_1s = 0.0002; // positive = aligned with long
    frame.leader_correlation = 0.7;
    strategy.on_frame(frame);

    // Signals for breakout: Support behind -> Eating -> Burst
    Signal s_support{SignalKind::DensityDetected, now - std::chrono::seconds(10), "BTCUSDT", 49980.0, 1.0, {.side = "Bid"}};
    Signal s_eating{SignalKind::DensityEating, now, "BTCUSDT", price, 1.0, {.side = "Ask", .size = 400.0, .original_size = 1000.0}};
    Signal s_burst{SignalKind::TapeBurst, now, "BTCUSDT", price, 1.0, {.side = "Buy", .ratio = 2.0}};

    strategy.on_signal(s_support);
    strategy.on_signal(s_eating);
    strategy.on_signal(s_burst);

    auto plan = strategy.tick(now);

    ASSERT_TRUE(plan.has_value());
    EXPECT_EQ(plan->side, Side::Buy);
    EXPECT_EQ(plan->entry_type, OrderType::Market);
    EXPECT_GT(plan->entry_price, price); // Aggressive offset applied
    EXPECT_LT(plan->stop_price, price);
    EXPECT_GT(plan->tp1_price, plan->entry_price);
}

TEST_F(BreakoutStrategyTest, NoPlanWithoutSupport) {
    TickerInfo info{"BTCUSDT", "BTC", "USDT", true, 0.01, 1e-6, 0.0, 0.0};
    BreakoutEatThrough strategy("BTCUSDT", info);
    auto now = std::chrono::system_clock::now();
    double price = 50000.0;
    
    strategy.on_signal({SignalKind::DensityEating, now, "BTCUSDT", price, 1.0, {.side = "Ask"}});
    strategy.on_signal({SignalKind::TapeBurst, now, "BTCUSDT", price, 1.0, {.side = "Buy"}});
    
    auto plan = strategy.tick(now);
    EXPECT_FALSE(plan.has_value());
}
