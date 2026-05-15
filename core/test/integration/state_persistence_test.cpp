#include "risk/AccountStatePersister.hpp"
#include <gtest/gtest.h>
#include <filesystem>

using namespace trade_bot;

TEST(StatePersistenceTest, SaveAndLoadMatch) {
    const std::string path = "test_persist.json";
    if (std::filesystem::exists(path)) std::filesystem::remove(path);
    
    AccountStatePersister persister(path);
    
    AccountStatePersister::PersistedData data;
    data.last_reset_day_utc = "2026-05-02";
    data.account_state.equity_usd = 12345.67;
    data.account_state.active_positions = 2;
    data.account_state.active_tickers = {"BTCUSDT", "ETHUSDT"};
    
    ActiveTrade t;
    t.plan.ticker = "BTCUSDT";
    t.plan.strategy_name = "Bounce";
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
    
    std::filesystem::remove(path);
}
