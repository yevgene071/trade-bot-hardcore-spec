#include "logger/Logger.hpp"
#include "signals/SignalBus.hpp"
#include "strategy/LeaderLag.hpp"

#include <gtest/gtest.h>

using namespace trade_bot;

namespace {

TickerInfo make_info() {
    return TickerInfo{"ALTUSDT", "ALT", "USDT", true, 0.01, 1e-6, 0.0, 0.0};
}

FeatureFrame make_frame(std::chrono::system_clock::time_point now) {
    FeatureFrame frame{};
    frame.ticker = "ALTUSDT";
    frame.mid = 100.0;
    frame.best_bid = 99.99;
    frame.best_ask = 100.01;
    frame.spread_bps = 2.0;
    frame.timestamp = now;
    frame.valid = true;
    return frame;
}

Signal leader_move(std::chrono::system_clock::time_point ts, double lag_pct = 0.25,
                   double correlation = 0.8) {
    return Signal{SignalKind::LeaderMove,
                  ts,
                  "ALTUSDT",
                  100.0,
                  0.8,
                  {.lag_pct = lag_pct, .correlation = correlation}};
}

Signal density(std::chrono::system_clock::time_point ts, double price, FixedString<8> side) {
    return Signal{SignalKind::DensityDetected,         ts, "ALTUSDT", price, 1.0,
                  {.side = side, .size_usd = 100000.0}};
}

} // namespace

class LeaderLagStrategyTest : public ::testing::Test {
  protected:
    void SetUp() override {
        Logger::init();
    }
};

TEST_F(LeaderLagStrategyTest, GeneratesLongPlanOnPositiveLag) {
    LeaderLag strategy("ALTUSDT", make_info());
    auto now = std::chrono::system_clock::now();

    strategy.on_frame(make_frame(now));
    strategy.on_signal(leader_move(now), now);

    auto plan = strategy.tick(now);

    ASSERT_TRUE(plan.has_value());
    EXPECT_EQ(plan->side, Side::Buy);
    EXPECT_EQ(plan->entry_type, OrderType::Market);
    EXPECT_GT(plan->tp1_price, 100.0);
}

TEST_F(LeaderLagStrategyTest, NoPlanOnLowCorrelation) {
    LeaderLag strategy("ALTUSDT", make_info());
    auto now = std::chrono::system_clock::now();

    strategy.on_frame(make_frame(now));
    strategy.on_signal(leader_move(now, 0.25, 0.4), now);

    auto plan = strategy.tick(now);
    EXPECT_FALSE(plan.has_value());
}

TEST_F(LeaderLagStrategyTest, RejectsStaleLeaderMove) {
    LeaderLag strategy("ALTUSDT", make_info());
    auto now = std::chrono::system_clock::now();

    strategy.on_frame(make_frame(now));
    strategy.on_signal(leader_move(now - std::chrono::milliseconds(3001)), now);

    auto plan = strategy.tick(now);
    EXPECT_FALSE(plan.has_value());
}

TEST_F(LeaderLagStrategyTest, RejectsAskDensityOnLongCatchupPath) {
    LeaderLag strategy("ALTUSDT", make_info());
    auto now = std::chrono::system_clock::now();

    strategy.on_frame(make_frame(now));
    strategy.on_signal(density(now - std::chrono::seconds(1), 100.10, "Ask"), now);
    strategy.on_signal(leader_move(now, 0.25, 0.8), now);

    auto plan = strategy.tick(now);
    EXPECT_FALSE(plan.has_value());
}

TEST_F(LeaderLagStrategyTest, RejectsBidDensityOnShortCatchupPath) {
    LeaderLag strategy("ALTUSDT", make_info());
    auto now = std::chrono::system_clock::now();

    strategy.on_frame(make_frame(now));
    strategy.on_signal(density(now - std::chrono::seconds(1), 99.90, "Bid"), now);
    strategy.on_signal(leader_move(now, -0.25, 0.8), now);

    auto plan = strategy.tick(now);
    EXPECT_FALSE(plan.has_value());
}

TEST_F(LeaderLagStrategyTest, DensityReleaseSpotFuturesVariantRemainsPhaseLater) {
    LeaderLag strategy("ALTUSDT", make_info());
    auto now = std::chrono::system_clock::now();

    strategy.on_frame(make_frame(now));
    strategy.on_signal(density(now - std::chrono::seconds(1), 100.10, "Ask"), now);
    strategy.on_signal(Signal{SignalKind::DensityEating,
                              now,
                              "ALTUSDT",
                              100.10,
                              1.0,
                              {.side = "Ask", .eaten_ratio = 0.75}},
                       now);
    strategy.on_signal(leader_move(now, 0.25, 0.8), now);

    // FN-004: manual density/robot-held release and spot/futures variants are
    // phase-later. Existing LeaderLag must not silently treat DensityEating as
    // a release permission while a blocking density remains on the path.
    auto plan = strategy.tick(now);
    EXPECT_FALSE(plan.has_value());
}

TEST(LeaderLagStrategyTest, LeaderLagPlansDisableFollowThroughByDefault) {
    TradePlan plan;
    EXPECT_DOUBLE_EQ(plan.min_follow_through_bps, 0.0);
    EXPECT_DOUBLE_EQ(plan.post_entry_grace_sec, 0.0);
}
