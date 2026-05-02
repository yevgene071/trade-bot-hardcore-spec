#include "strategy/LeaderLag.hpp"
#include "signals/SignalBus.hpp"
#include "logger/Logger.hpp"
#include <gtest/gtest.h>

using namespace trade_bot;

class LeaderLagStrategyTest : public ::testing::Test {
protected:
    void SetUp() override { Logger::init(); }
};

TEST_F(LeaderLagStrategyTest, GeneratesLongPlanOnPositiveLag) {
    LeaderLag strategy("ALTUSDT");
    auto now = std::chrono::system_clock::now();
    
    FeatureFrame frame{};
    frame.ticker = "ALTUSDT";
    frame.mid = 100.0;
    frame.best_bid = 99.99;
    frame.best_ask = 100.01;
    frame.timestamp = now;
    strategy.on_frame(frame);
    
    Signal s{
        SignalKind::LeaderMove, now, "ALTUSDT", 100.0, 0.8,
        {{"lag_pct", 0.25}, {"correlation", 0.8}}
    };
    strategy.on_signal(s);
    
    auto plan = strategy.tick(now);
    
    ASSERT_TRUE(plan.has_value());
    EXPECT_EQ(plan->side, Side::Buy);
    EXPECT_EQ(plan->entry_type, OrderType::Market);
    EXPECT_GT(plan->tp1_price, 100.0);
}

TEST_F(LeaderLagStrategyTest, NoPlanOnLowCorrelation) {
    LeaderLag strategy("ALTUSDT");
    auto now = std::chrono::system_clock::now();
    
    Signal s{
        SignalKind::LeaderMove, now, "ALTUSDT", 100.0, 0.8,
        {{"lag_pct", 0.25}, {"correlation", 0.4}} // low
    };
    strategy.on_signal(s);
    
    auto plan = strategy.tick(now);
    EXPECT_FALSE(plan.has_value());
}
