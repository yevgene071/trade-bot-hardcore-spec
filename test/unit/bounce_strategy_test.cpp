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
    TickerInfo info{"BTCUSDT", "BTC", "USDT", true, 0.01, 1e-6, 0.0, 0.0};
    BounceFromDensity strategy("BTCUSDT", info);

    auto now = std::chrono::system_clock::now();
    double price = 50000.0;

    // Feed a FeatureFrame with required fields for strategy checks
    FeatureFrame frame;
    frame.ticker = "BTCUSDT";
    frame.timestamp = now;
    frame.mid = price;
    frame.best_bid = price - 0.5;
    frame.best_ask = price + 0.5;
    frame.bid_depth_10 = 100.0;
    frame.ask_depth_10 = 100.0;
    frame.buy_vol_1s = 5.0;
    frame.buy_vol_5s = 30.0;
    frame.sell_vol_5s = 30.0;
    frame.leader_change_1s = 0.0001; // small positive = aligned with long
    frame.price_change_1s = 0.0;
    strategy.on_frame(frame);

    // Signals for bounce: Approach -> Density -> Fade
    Signal s1{SignalKind::LevelApproach, now, "BTCUSDT", price, 1.0, {.speed_bps = 10.0, .approach_type = "impulse"}};
    Signal s2{SignalKind::DensityDetected, now, "BTCUSDT", price, 1.0, {.side = "Bid", .size = 500.0}};
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
    TickerInfo info{"BTCUSDT", "BTC", "USDT", true, 0.01, 1e-6, 0.0, 0.0};
    BounceFromDensity strategy("BTCUSDT", info);
    auto now = std::chrono::system_clock::now();
    double price = 50000.0;

    // Feed FeatureFrame
    FeatureFrame frame;
    frame.ticker = "BTCUSDT";
    frame.timestamp = now;
    frame.mid = price;
    frame.best_bid = price - 0.5;
    frame.best_ask = price + 0.5;
    frame.bid_depth_10 = 100.0;
    frame.ask_depth_10 = 100.0;
    frame.buy_vol_1s = 5.0;
    frame.buy_vol_5s = 30.0;
    frame.sell_vol_5s = 30.0;
    frame.leader_change_1s = 0.0001;
    frame.price_change_1s = 0.0;
    strategy.on_frame(frame);

    strategy.on_signal({SignalKind::LevelApproach, now, "BTCUSDT", price, 1.0, {.speed_bps = 10.0, .approach_type = "impulse"}});
    strategy.on_signal({SignalKind::DensityDetected, now, "BTCUSDT", price, 1.0, {.side = "Bid", .size = 500.0}});
    strategy.on_signal({SignalKind::TapeFade, now, "BTCUSDT", price, 1.0, {}});

    // Before tick, an iceberg appears on the same level
    strategy.on_signal({SignalKind::IcebergSuspected, now, "BTCUSDT", price, 1.0, {.side = "Bid"}});

    auto plan = strategy.tick(now);
    EXPECT_FALSE(plan.has_value());
}
