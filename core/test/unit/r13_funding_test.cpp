#include "risk/RiskManager.hpp"
#include <gtest/gtest.h>

using namespace trade_bot;

TEST(R13FundingTest, RejectsInsideBlackoutWindow) {
    TickerUniverse universe;
    NewsCalendar news;
    RiskManager risk(universe, news);
    
    auto now = std::chrono::system_clock::now();
    risk.update_funding_time("BTCUSDT", now + std::chrono::seconds(10));
    
    TradePlan plan;
    plan.ticker = "BTCUSDT";
    plan.side = Side::Buy;
    plan.entry_price = 50000.0;
    plan.stop_price = 49950.0; // 10 bps
    plan.tp1_price = 50100.0;  // 20 bps (2R)
    
    AccountState state;
    state.starting_equity_usd = 10000.0;
    state.equity_usd = 10000.0;
    state.free_balance_usd = 10000.0;
    
    auto decision = risk.evaluate(plan, state);
    EXPECT_FALSE(decision.accepted) << decision.details;
    EXPECT_EQ(decision.reason, RejectReason::FundingBlackout);
}

TEST(R13FundingTest, PassesOutsideBlackoutWindow) {
    TickerUniverse universe;
    NewsCalendar news;
    RiskManager risk(universe, news);
    
    auto now = std::chrono::system_clock::now();
    risk.update_funding_time("BTCUSDT", now + std::chrono::seconds(61)); // > 30s
    
    TradePlan plan;
    plan.ticker = "BTCUSDT";
    plan.side = Side::Buy;
    plan.entry_price = 50000.0;
    plan.stop_price = 49950.0; // 10 bps
    plan.tp1_price = 50100.0;
    
    AccountState state;
    state.starting_equity_usd = 10000.0;
    state.equity_usd = 10000.0;
    state.free_balance_usd = 10000.0;
    
    auto decision = risk.evaluate(plan, state);
    EXPECT_TRUE(decision.accepted) << decision.details;
}
