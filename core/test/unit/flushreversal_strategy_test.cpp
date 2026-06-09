#include "logger/Logger.hpp"
#include "strategy/FlushReversal.hpp"

#include <gtest/gtest.h>

using namespace trade_bot;

namespace {

FeatureFrame make_frame(const Ticker& ticker, std::chrono::system_clock::time_point now) {
    FeatureFrame frame;
    frame.ticker = ticker;
    frame.timestamp = now;
    frame.valid = true;
    frame.mid = 50000.0;
    frame.best_bid = 49999.5;
    frame.best_ask = 50000.5;
    frame.spread_bps = 1.0;
    frame.sell_vol_1s = 10.0;
    frame.sell_vol_5s = 200.0;
    frame.buy_vol_1s = 5.0;
    frame.buy_vol_5s = 100.0;
    frame.price_change_1s = 0.10; // 10 bps upward reversal for a flushed-down level.
    return frame;
}

Signal tape_flush(std::chrono::system_clock::time_point ts) {
    return Signal{.kind = SignalKind::TapeFlush,
                  .timestamp = ts,
                  .ticker = "BTCUSDT",
                  .price = 49970.0,
                  .confidence = 1.0,
                  .payload = {.side = "Sell", .delta_bps = -6.0}};
}

Signal level_break(std::chrono::system_clock::time_point ts) {
    return Signal{.kind = SignalKind::LevelBreak,
                  .timestamp = ts,
                  .ticker = "BTCUSDT",
                  .price = 50000.0,
                  .confidence = 1.0,
                  .payload = {.dist_bps = -5.0, .touches = 2}};
}

Signal continuation_burst(std::chrono::system_clock::time_point ts) {
    return Signal{.kind = SignalKind::TapeBurst,
                  .timestamp = ts,
                  .ticker = "BTCUSDT",
                  .price = 49960.0,
                  .confidence = 1.0,
                  .payload = {.side = "Sell"}};
}

Signal continuation_density_eating(std::chrono::system_clock::time_point ts) {
    return Signal{.kind = SignalKind::DensityEating,
                  .timestamp = ts,
                  .ticker = "BTCUSDT",
                  .price = 49960.0,
                  .confidence = 1.0,
                  .payload = {.side = "Ask", .eaten_ratio = 0.6}};
}

void seed_repeated_flushes(FlushReversal& strategy, std::chrono::system_clock::time_point now) {
    strategy.on_signal(tape_flush(now - std::chrono::seconds(2)), now);
    strategy.on_signal(tape_flush(now - std::chrono::seconds(1)), now);
}

} // namespace

class FlushReversalStrategyTest : public ::testing::Test {
  protected:
    void SetUp() override {
        Logger::init();
    }
};

TEST_F(FlushReversalStrategyTest, RejectsSingleTapeFlushByDefault) {
    TickerInfo info{"BTCUSDT", "BTC", "USDT", true, 0.01, 1e-6, 0.0, 0.0};
    FlushReversal strategy("BTCUSDT", info);
    const auto now = std::chrono::system_clock::now();

    strategy.on_frame(make_frame("BTCUSDT", now));
    strategy.on_signal(tape_flush(now - std::chrono::seconds(1)), now);

    auto plan = strategy.on_signal(level_break(now), now);

    EXPECT_FALSE(plan.has_value());
}

TEST_F(FlushReversalStrategyTest, GeneratesPlanAfterRepeatedTapeFlushes) {
    TickerInfo info{"BTCUSDT", "BTC", "USDT", true, 0.01, 1e-6, 0.0, 0.0};
    FlushReversal strategy("BTCUSDT", info);
    const auto now = std::chrono::system_clock::now();

    strategy.on_frame(make_frame("BTCUSDT", now));
    seed_repeated_flushes(strategy, now);

    auto plan = strategy.on_signal(level_break(now), now);

    ASSERT_TRUE(plan.has_value());
    EXPECT_EQ(plan->strategy_name, "FlushReversal");
    EXPECT_EQ(plan->side, Side::Buy);
    EXPECT_EQ(plan->entry_type, OrderType::Limit);
    EXPECT_LT(plan->stop_price, plan->entry_price);
    EXPECT_GT(plan->tp1_price, plan->entry_price);
}

TEST_F(FlushReversalStrategyTest, RejectsContinuationTapeBurst) {
    TickerInfo info{"BTCUSDT", "BTC", "USDT", true, 0.01, 1e-6, 0.0, 0.0};
    FlushReversal strategy("BTCUSDT", info);
    const auto now = std::chrono::system_clock::now();

    strategy.on_frame(make_frame("BTCUSDT", now));
    seed_repeated_flushes(strategy, now);
    strategy.on_signal(continuation_burst(now - std::chrono::seconds(1)), now);

    auto plan = strategy.on_signal(level_break(now), now);
    EXPECT_FALSE(plan.has_value());
}

TEST_F(FlushReversalStrategyTest, RejectsContinuationDensityEating) {
    TickerInfo info{"BTCUSDT", "BTC", "USDT", true, 0.01, 1e-6, 0.0, 0.0};
    FlushReversal strategy("BTCUSDT", info);
    const auto now = std::chrono::system_clock::now();

    strategy.on_frame(make_frame("BTCUSDT", now));
    seed_repeated_flushes(strategy, now);
    strategy.on_signal(continuation_density_eating(now - std::chrono::seconds(1)), now);

    auto plan = strategy.on_signal(level_break(now), now);
    EXPECT_FALSE(plan.has_value());
}

TEST_F(FlushReversalStrategyTest, PlainTapeFlushDoesNotSatisfyLiveGates) {
    TickerInfo info{"BTCUSDT", "BTC", "USDT", true, 0.01, 1e-6, 0.0, 0.0};
    FlushReversal::Config cfg;
    cfg.allow_live = true;
    cfg.live_gates_implemented = false;
    cfg.require_liquidation_flush = true;
    cfg.require_open_interest_confirmation = true;
    FlushReversal strategy("BTCUSDT", info, cfg);
    const auto now = std::chrono::system_clock::now();

    strategy.on_frame(make_frame("BTCUSDT", now));
    seed_repeated_flushes(strategy, now);

    auto plan = strategy.on_signal(level_break(now), now);

    ASSERT_TRUE(plan.has_value()); // paper/offline prototype still works
    EXPECT_EQ(plan->strategy_name, "FlushReversal");
    EXPECT_TRUE(cfg.require_liquidation_flush);
    EXPECT_TRUE(cfg.require_open_interest_confirmation);
    EXPECT_FALSE(cfg.live_gates_implemented);
    EXPECT_EQ(plan->evidence.size(), 2U);
    EXPECT_EQ(plan->evidence[0].kind, SignalKind::LevelBreak);
    EXPECT_EQ(plan->evidence[1].kind, SignalKind::TapeFlush);
    EXPECT_FALSE(FlushReversal::live_gate_satisfied(*plan, cfg));
}

TEST_F(FlushReversalStrategyTest, LiveGateRejectsEvenIfManualFlagsAreTrueUntilT5EvidenceExists) {
    TradePlan plan;
    plan.strategy_name = "FlushReversal";
    plan.evidence = {tape_flush(std::chrono::system_clock::now()),
                     tape_flush(std::chrono::system_clock::now())};

    FlushReversal::Config cfg;
    cfg.allow_live = true;
    cfg.live_gates_implemented = true;
    cfg.require_liquidation_flush = true;
    cfg.require_open_interest_confirmation = true;

    EXPECT_FALSE(FlushReversal::live_gate_satisfied(plan, cfg));
}
