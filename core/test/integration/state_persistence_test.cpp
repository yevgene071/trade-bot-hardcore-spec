#include "risk/AccountStatePersister.hpp"

#include <filesystem>
#include <gtest/gtest.h>

using namespace trade_bot;

TEST(StatePersistenceTest, SaveAndLoadMatch) {
    const std::string path = "test_persist.json";
    if (std::filesystem::exists(path))
        std::filesystem::remove(path);

    AccountStatePersister persister(path);

    AccountStatePersister::PersistedData data;
    data.last_reset_day_utc = "2026-05-02";
    data.account_state.equity_usd = 12345.67;
    data.account_state.active_positions = 2;
    data.account_state.active_tickers = {"BTCUSDT", "ETHUSDT"};

    ActiveTrade t;
    t.plan.ticker = "BTCUSDT";
    t.plan.strategy_name = "Bounce";
    t.plan.tp2_price = 50500.0;
    t.plan.no_progress_timeout_sec = 42.0;
    t.plan.post_entry_grace_sec = 3.0;
    t.plan.min_follow_through_bps = 7.0;
    t.plan.entry_correlation = 0.81;
    t.plan.leader_entry_lag_pct = -0.12;
    t.plan.correlation_exit_threshold = 0.35;
    t.plan.leader_exit_reversal_bps = 4.5;
    t.plan.density_price_for_stop = 49980.0;
    t.plan.approach_count = 3;
    t.plan.trace_id = 12345;
    t.state = TradeState::Open;
    t.executed_size = 0.5;
    data.active_trades.push_back(t);

    persister.save(data);

    auto loaded = persister.load();
    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(loaded->last_reset_day_utc, "2026-05-02");
    EXPECT_DOUBLE_EQ(loaded->account_state.equity_usd, 12345.67);
    ASSERT_EQ(loaded->active_trades.size(), 1);
    EXPECT_EQ(loaded->active_trades[0].plan.ticker, "BTCUSDT");
    EXPECT_EQ(loaded->active_trades[0].state, TradeState::Open);
    const auto& plan = loaded->active_trades[0].plan;
    ASSERT_TRUE(plan.tp2_price.has_value());
    EXPECT_DOUBLE_EQ(*plan.tp2_price, 50500.0);
    EXPECT_DOUBLE_EQ(plan.no_progress_timeout_sec, 42.0);
    EXPECT_DOUBLE_EQ(plan.post_entry_grace_sec, 3.0);
    EXPECT_DOUBLE_EQ(plan.min_follow_through_bps, 7.0);
    EXPECT_DOUBLE_EQ(plan.entry_correlation, 0.81);
    EXPECT_DOUBLE_EQ(plan.leader_entry_lag_pct, -0.12);
    EXPECT_DOUBLE_EQ(plan.correlation_exit_threshold, 0.35);
    EXPECT_DOUBLE_EQ(plan.leader_exit_reversal_bps, 4.5);
    EXPECT_DOUBLE_EQ(plan.density_price_for_stop, 49980.0);
    EXPECT_EQ(plan.approach_count, 3);
    EXPECT_EQ(plan.trace_id, 12345);

    std::filesystem::remove(path);
}
