#include "logger/Logger.hpp"
#include "signals/SignalBus.hpp"
#include "strategy/BreakoutEatThrough.hpp"

#include <gtest/gtest.h>
#include <optional>

using namespace trade_bot;

namespace {

TickerInfo make_info() {
    return {"BTCUSDT", "BTC", "USDT", true, 0.01, 1e-6, 0.0, 0.0};
}

FeatureFrame make_breakout_frame(std::chrono::system_clock::time_point now) {
    constexpr double price = 50000.0;
    FeatureFrame frame;
    frame.ticker = "BTCUSDT";
    frame.timestamp = now;
    frame.mid = price;
    frame.best_bid = price - 20.0;
    frame.best_ask = price - 15.0;
    frame.buy_vol_5s = 200.0;
    frame.buy_vol_30s = 500.0;
    frame.sell_vol_5s = 50.0;
    frame.sell_vol_30s = 300.0;
    frame.tape_aggression = 0.4;
    frame.leader_change_1s = 0.0002;
    frame.leader_change_5s = 0.05;
    frame.leader_correlation = 0.6;
    return frame;
}

void seed_support_and_burst(BreakoutEatThrough& strategy, std::chrono::system_clock::time_point now,
                            double price = 50000.0) {
    Signal support{SignalKind::DensityDetected,
                   now - std::chrono::seconds(10),
                   "BTCUSDT",
                   49980.0,
                   1.0,
                   {.side = "Bid"}};
    Signal burst{SignalKind::TapeBurst, now, "BTCUSDT", price, 1.0, {.side = "Buy", .ratio = 2.0}};
    strategy.on_signal(support, now);
    strategy.on_signal(burst, now);
}

std::optional<TradePlan> trigger_long(BreakoutEatThrough& strategy,
                                      std::chrono::system_clock::time_point now,
                                      double remaining = 400.0, double original = 1000.0,
                                      double price = 50000.0) {
    Signal eating{SignalKind::DensityEating,
                  now,
                  "BTCUSDT",
                  price,
                  1.0,
                  {.side = "Ask", .original_size = original, .remaining_size = remaining}};
    return strategy.on_signal(eating, now);
}

void seed_short_support_and_burst(BreakoutEatThrough& strategy,
                                  std::chrono::system_clock::time_point now,
                                  double price = 50000.0) {
    Signal support{SignalKind::DensityDetected,
                   now - std::chrono::seconds(10),
                   "BTCUSDT",
                   50020.0,
                   1.0,
                   {.side = "Ask"}};
    Signal burst{SignalKind::TapeBurst, now, "BTCUSDT", price, 1.0, {.side = "Sell", .ratio = 2.0}};
    strategy.on_signal(support, now);
    strategy.on_signal(burst, now);
}

std::optional<TradePlan> trigger_short(BreakoutEatThrough& strategy,
                                       std::chrono::system_clock::time_point now,
                                       double remaining = 400.0, double original = 1000.0,
                                       double price = 50000.0) {
    Signal eating{SignalKind::DensityEating,
                  now,
                  "BTCUSDT",
                  price,
                  1.0,
                  {.side = "Bid", .original_size = original, .remaining_size = remaining}};
    return strategy.on_signal(eating, now);
}

std::string last_reject(const BreakoutEatThrough& strategy) {
    return strategy.get_state().last_reject_reason;
}

} // namespace

class BreakoutStrategyTest : public ::testing::Test {
  protected:
    void SetUp() override {
        Logger::init();
    }
};

TEST_F(BreakoutStrategyTest, GeneratesLongPlanFromAskEating) {
    BreakoutEatThrough strategy("BTCUSDT", make_info());
    auto now = std::chrono::system_clock::now();
    double price = 50000.0;

    strategy.on_frame(make_breakout_frame(now));
    seed_support_and_burst(strategy, now, price);
    auto plan = trigger_long(strategy, now);

    ASSERT_TRUE(plan.has_value());
    EXPECT_EQ(plan->side, Side::Buy);
    EXPECT_EQ(plan->entry_type, OrderType::Market);
    EXPECT_GT(plan->entry_price, price); // Aggressive offset applied
    EXPECT_LT(plan->stop_price, price);
    EXPECT_GT(plan->tp1_price, plan->entry_price);
    EXPECT_EQ(plan->strategy_name, "BreakoutEatThrough");
    EXPECT_EQ(plan->reason, "Breakout through Ask density");
    ASSERT_EQ(plan->evidence.size(), 2);
    EXPECT_EQ(plan->evidence[0].kind, SignalKind::DensityEating);
    EXPECT_EQ(plan->evidence[1].kind, SignalKind::TapeBurst);
    const double risk_per_coin = std::abs(plan->entry_price - plan->stop_price);
    const double reward_per_coin = std::abs(plan->tp1_price - plan->entry_price);
    EXPECT_GE(reward_per_coin + 1e-12, risk_per_coin);
}

TEST_F(BreakoutStrategyTest, NoPlanWithoutSupport) {
    BreakoutEatThrough strategy("BTCUSDT", make_info());
    auto now = std::chrono::system_clock::now();
    double price = 50000.0;

    strategy.on_frame(make_breakout_frame(now));
    strategy.on_signal({SignalKind::TapeBurst, now, "BTCUSDT", price, 1.0, {.side = "Buy"}}, now);

    auto plan = trigger_long(strategy, now);
    EXPECT_FALSE(plan.has_value());
    EXPECT_EQ(last_reject(strategy), "WeakBreakoutNoSupportBehind");
}

TEST_F(BreakoutStrategyTest, RejectsLowParticipationWithStableReason) {
    BreakoutEatThrough strategy("BTCUSDT", make_info());
    auto now = std::chrono::system_clock::now();
    auto frame = make_breakout_frame(now);
    frame.buy_vol_5s = 10.0;
    frame.buy_vol_30s = 500.0;
    strategy.on_frame(frame);
    seed_support_and_burst(strategy, now);

    EXPECT_FALSE(trigger_long(strategy, now).has_value());
    EXPECT_EQ(last_reject(strategy), "WeakBreakoutLowParticipation");
}

TEST_F(BreakoutStrategyTest, RejectsContraResistanceClusterWithStableReason) {
    BreakoutEatThrough strategy("BTCUSDT", make_info());
    auto now = std::chrono::system_clock::now();
    strategy.on_frame(make_breakout_frame(now));
    seed_support_and_burst(strategy, now);
    Signal resistance{SignalKind::DensityDetected,   now, "BTCUSDT", 50020.0, 1.0,
                      {.side = "Ask", .size = 800.0}};
    strategy.on_signal(resistance, now);

    EXPECT_FALSE(trigger_long(strategy, now).has_value());
    EXPECT_EQ(last_reject(strategy), "WeakBreakoutContraResistanceCluster");
}

TEST_F(BreakoutStrategyTest, RejectsTapeFadeWithStableReason) {
    BreakoutEatThrough strategy("BTCUSDT", make_info());
    auto now = std::chrono::system_clock::now();
    strategy.on_frame(make_breakout_frame(now));
    seed_support_and_burst(strategy, now);
    Signal fade{SignalKind::TapeFade,
                now,
                "BTCUSDT",
                50000.0,
                1.0,
                {.peak_rate = 100.0, .current_rate = 20.0, .cusum = 5.0}};
    strategy.on_signal(fade, now);

    EXPECT_FALSE(trigger_long(strategy, now).has_value());
    EXPECT_EQ(last_reject(strategy), "WeakBreakoutTapeFaded");
}

TEST_F(BreakoutStrategyTest, RejectsLeaderContraWithStableReason) {
    BreakoutEatThrough strategy("BTCUSDT", make_info());
    auto now = std::chrono::system_clock::now();
    auto frame = make_breakout_frame(now);
    frame.leader_change_5s = -0.20;
    strategy.on_frame(frame);
    seed_support_and_burst(strategy, now);

    EXPECT_FALSE(trigger_long(strategy, now).has_value());
    EXPECT_EQ(last_reject(strategy), "WeakBreakoutLeaderContra");
}

TEST_F(BreakoutStrategyTest, RejectsTooCloseToBestWithStableReason) {
    BreakoutEatThrough strategy("BTCUSDT", make_info());
    auto now = std::chrono::system_clock::now();
    auto frame = make_breakout_frame(now);
    frame.best_ask = 49995.0; // 1 bps from density, below default 2 bps.
    strategy.on_frame(frame);
    seed_support_and_burst(strategy, now);

    EXPECT_FALSE(trigger_long(strategy, now).has_value());
    EXPECT_EQ(last_reject(strategy), "WeakBreakoutTooCloseToBest");
}

TEST_F(BreakoutStrategyTest, RejectsObstacleBeforeTp1WithStableReason) {
    BreakoutEatThrough strategy("BTCUSDT", make_info());
    auto now = std::chrono::system_clock::now();
    auto frame = make_breakout_frame(now);
    strategy.on_frame(frame);
    seed_support_and_burst(strategy, now);
    Signal obstacle{SignalKind::DensityDetected,   now, "BTCUSDT", 50030.0, 1.0,
                    {.side = "Ask", .size = 250.0}};
    strategy.on_signal(obstacle, now);

    EXPECT_FALSE(trigger_long(strategy, now).has_value());
    EXPECT_EQ(last_reject(strategy), "PotentialObstacleBeforeTp1");
}

TEST_F(BreakoutStrategyTest, RejectsLeaderMoveContraWithConfiguredThreshold) {
    BreakoutEatThrough::Config cfg;
    cfg.leader_pre_entry_contra_pct = 0.20;
    BreakoutEatThrough strategy("BTCUSDT", make_info(), cfg);
    auto now = std::chrono::system_clock::now();
    strategy.on_frame(make_breakout_frame(now));
    seed_support_and_burst(strategy, now);

    Signal small_contra{SignalKind::LeaderMove, now, "BTCUSDT", 50000.0, 1.0, {.lag_pct = -0.09}};
    strategy.on_signal(small_contra, now);
    EXPECT_TRUE(trigger_long(strategy, now).has_value());

    strategy.reset_active_plan();
    Signal large_contra{SignalKind::LeaderMove, now, "BTCUSDT", 50000.0, 1.0, {.lag_pct = -0.21}};
    strategy.on_signal(large_contra, now);
    EXPECT_FALSE(trigger_long(strategy, now).has_value());
    EXPECT_EQ(last_reject(strategy), "WeakBreakoutLeaderContra");
}

TEST_F(BreakoutStrategyTest, RejectsTp1BelowOneRWithStableReason) {
    BreakoutEatThrough::Config cfg;
    cfg.tp1_r = 0.9;
    BreakoutEatThrough strategy("BTCUSDT", make_info(), cfg);
    auto now = std::chrono::system_clock::now();
    strategy.on_frame(make_breakout_frame(now));
    seed_support_and_burst(strategy, now);

    EXPECT_FALSE(trigger_long(strategy, now).has_value());
    EXPECT_EQ(last_reject(strategy), "PotentialTp1BelowOneR");
}

TEST_F(BreakoutStrategyTest, RejectsSupportAheadOfBreakoutDensity) {
    BreakoutEatThrough strategy("BTCUSDT", make_info());
    auto now = std::chrono::system_clock::now();
    strategy.on_frame(make_breakout_frame(now));
    Signal ahead_support{SignalKind::DensityDetected,
                         now - std::chrono::seconds(10),
                         "BTCUSDT",
                         50010.0,
                         1.0,
                         {.side = "Bid"}};
    Signal burst{SignalKind::TapeBurst,        now, "BTCUSDT", 50000.0, 1.0,
                 {.side = "Buy", .ratio = 2.0}};
    strategy.on_signal(ahead_support, now);
    strategy.on_signal(burst, now);

    EXPECT_FALSE(trigger_long(strategy, now).has_value());
    EXPECT_EQ(last_reject(strategy), "WeakBreakoutNoSupportBehind");
}

TEST_F(BreakoutStrategyTest, RejectsFullyEatenDensityUsingCanonicalRemainingSize) {
    BreakoutEatThrough strategy("BTCUSDT", make_info());
    auto now = std::chrono::system_clock::now();
    strategy.on_frame(make_breakout_frame(now));
    seed_support_and_burst(strategy, now);
    Signal eating{SignalKind::DensityEating,
                  now,
                  "BTCUSDT",
                  50000.0,
                  1.0,
                  {.side = "Ask", .size = 400.0, .original_size = 1000.0, .remaining_size = 0.0}};

    EXPECT_FALSE(strategy.on_signal(eating, now).has_value());
    EXPECT_EQ(last_reject(strategy), "WeakBreakoutDensityFullyEaten");
}

TEST_F(BreakoutStrategyTest, ClosesPostEntryOnLeaderContraPercentThreshold) {
    BreakoutEatThrough strategy("BTCUSDT", make_info());
    auto now = std::chrono::system_clock::now();
    auto frame = make_breakout_frame(now);
    strategy.on_frame(frame);
    seed_support_and_burst(strategy, now);
    auto plan = trigger_long(strategy, now);
    ASSERT_TRUE(plan.has_value());
    strategy.on_plan_accepted(*plan);
    Signal small_contra{SignalKind::LeaderMove, now, "BTCUSDT", 50000.0, 1.0,
                        {.lag_pct = -0.14}}; // 0.14% contra <= 0.15% default
    strategy.on_signal(small_contra, now);
    EXPECT_FALSE(strategy.check_close_conditions(frame).has_value());

    Signal leader_contra{SignalKind::LeaderMove, now, "BTCUSDT", 50000.0, 1.0,
                         {.lag_pct = -0.16}}; // 0.16% contra > 0.15% default
    strategy.on_signal(leader_contra, now);

    auto reason = strategy.check_close_conditions(frame);
    ASSERT_TRUE(reason.has_value());
    EXPECT_EQ(std::string_view(*reason), "LeaderContraPostEntry");
}

TEST_F(BreakoutStrategyTest, RejectsMissingParticipationBaseline) {
    BreakoutEatThrough strategy("BTCUSDT", make_info());
    auto now = std::chrono::system_clock::now();
    auto frame = make_breakout_frame(now);
    frame.buy_vol_30s = 0.0;
    strategy.on_frame(frame);
    seed_support_and_burst(strategy, now);

    EXPECT_FALSE(trigger_long(strategy, now).has_value());
    EXPECT_EQ(last_reject(strategy), "WeakBreakoutLowParticipation");
}

TEST_F(BreakoutStrategyTest, ClosesPostEntryOnDetectorShapedTapeFade) {
    BreakoutEatThrough strategy("BTCUSDT", make_info());
    auto now = std::chrono::system_clock::now();
    auto frame = make_breakout_frame(now);
    strategy.on_frame(frame);
    seed_support_and_burst(strategy, now);
    auto plan = trigger_long(strategy, now);
    ASSERT_TRUE(plan.has_value());
    strategy.on_plan_accepted(*plan);
    Signal fade{SignalKind::TapeFade,
                now,
                "BTCUSDT",
                50000.0,
                1.0,
                {.peak_rate = 100.0, .current_rate = 20.0, .cusum = 5.0}};
    strategy.on_signal(fade, now);

    auto reason = strategy.check_close_conditions(frame);
    ASSERT_TRUE(reason.has_value());
    EXPECT_EQ(std::string_view(*reason), "FadeOnOurSidePostEntry");
}

TEST_F(BreakoutStrategyTest, AllowsNeutralCorrelatedLeaderMove) {
    BreakoutEatThrough strategy("BTCUSDT", make_info());
    auto now = std::chrono::system_clock::now();
    auto frame = make_breakout_frame(now);
    frame.leader_correlation = 0.6;
    frame.leader_change_5s = -0.09; // neutral: within default 0.10% threshold
    strategy.on_frame(frame);
    seed_support_and_burst(strategy, now);

    EXPECT_TRUE(trigger_long(strategy, now).has_value());
}

TEST_F(BreakoutStrategyTest, GeneratesShortPlanFromBidEating) {
    BreakoutEatThrough strategy("BTCUSDT", make_info());
    auto now = std::chrono::system_clock::now();
    double price = 50000.0;
    auto frame = make_breakout_frame(now);
    frame.best_bid = price + 15.0; // Bid is above density; distance = 3 bps > 2 bps threshold
    frame.best_ask = price + 20.0;
    frame.sell_vol_5s = 200.0;
    frame.sell_vol_30s = 500.0;
    frame.buy_vol_5s = 50.0;
    frame.buy_vol_30s = 300.0;
    frame.tape_aggression = -0.4;
    frame.leader_change_5s = -0.05;
    strategy.on_frame(frame);
    seed_short_support_and_burst(strategy, now, price);

    auto plan = trigger_short(strategy, now);
    ASSERT_TRUE(plan.has_value());
    EXPECT_EQ(plan->side, Side::Sell);
    EXPECT_LT(plan->entry_price, price);
    EXPECT_GT(plan->stop_price, price);
    EXPECT_LT(plan->tp1_price, plan->entry_price);
    EXPECT_EQ(plan->reason, "Breakout through Bid density");
    ASSERT_EQ(plan->evidence.size(), 2);
    EXPECT_EQ(plan->evidence[0].kind, SignalKind::DensityEating);
    EXPECT_EQ(plan->evidence[1].kind, SignalKind::TapeBurst);
}

TEST_F(BreakoutStrategyTest, RejectsShortObstacleBeforeTp1WithStableReason) {
    BreakoutEatThrough strategy("BTCUSDT", make_info());
    auto now = std::chrono::system_clock::now();
    double price = 50000.0;
    auto frame = make_breakout_frame(now);
    frame.best_bid = price + 15.0;
    frame.best_ask = price + 20.0;
    frame.sell_vol_5s = 200.0;
    frame.sell_vol_30s = 500.0;
    frame.tape_aggression = -0.4;
    frame.leader_change_5s = -0.05;
    strategy.on_frame(frame);
    seed_short_support_and_burst(strategy, now, price);
    Signal obstacle{SignalKind::DensityDetected,   now, "BTCUSDT", 49970.0, 1.0,
                    {.side = "Bid", .size = 250.0}};
    strategy.on_signal(obstacle, now);

    EXPECT_FALSE(trigger_short(strategy, now).has_value());
    EXPECT_EQ(last_reject(strategy), "PotentialObstacleBeforeTp1");
}
