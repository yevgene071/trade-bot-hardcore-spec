#include "risk/RiskManager.hpp"
#include "logger/Logger.hpp"
#include <gtest/gtest.h>

using namespace trade_bot;

TEST(RiskFlowTest, DaySimulationWithLossStreak) {
    Logger::init();   // record_trade_end now logs WARN on streak — needs spdlog
    TickerUniverse universe;
    NewsCalendar news;
    RiskManager::Config cfg;
    cfg.max_consecutive_losses = 2;
    cfg.loss_streak_cooloff_min = 10;
    cfg.whitelist_tickers = {"BTCUSDT"};
    RiskManager risk(universe, news, cfg);
    
    AccountState state;
    state.starting_equity_usd = 10000.0;
    state.equity_usd = 10000.0;
    state.free_balance_usd = 10000.0;
    
    TradePlan plan;
    plan.ticker = "BTC_USDT";
    plan.side = Side::Buy;
    plan.entry_price = 50000.0;
    plan.stop_price = 49900.0;
    plan.tp1_price = 51000.0;
    
    auto now = std::chrono::system_clock::now();
    
    // 1. First trade - loss
    EXPECT_TRUE(risk.evaluate(plan, state).accepted);
    risk.record_trade_end(true, now);
    
    // 2. Second trade - loss
    EXPECT_TRUE(risk.evaluate(plan, state).accepted);
    risk.record_trade_end(true, now + std::chrono::minutes(1));
    
    // 3. Third trade - should be rejected by loss streak
    auto decision = risk.evaluate(plan, state);
    EXPECT_FALSE(decision.accepted);
    EXPECT_EQ(decision.reason, RejectReason::LossStreakCircuitBreaker);
    
    // 4. Wait for cooloff
    decision = risk.evaluate(plan, state); // Still blocked
    EXPECT_FALSE(decision.accepted);
    
    // Manual wait (simulated)
    // In evaluate() we use now = system_clock::now(), so we can't easily jump time without mocking.
    // I'll skip the actual wait in this unit-like integration test.
}
