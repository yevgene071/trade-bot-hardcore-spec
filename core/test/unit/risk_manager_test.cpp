#include "logger/Logger.hpp"
#include "risk/RiskManager.hpp"

#include <gtest/gtest.h>

using namespace trade_bot;

class RiskManagerTest : public ::testing::Test {
  protected:
    void SetUp() override {
        Logger::init();
    }

    AccountState make_state() {
        AccountState s;
        s.equity_usd = 10000.0;
        s.starting_equity_usd = 10000.0;
        s.free_balance_usd = 10000.0;
        return s;
    }

    TradePlan make_plan() {
        TradePlan p;
        p.ticker = "BTCUSDT";
        p.side = Side::Buy;
        p.entry_price = 50000.0;
        p.stop_price = 49950.0; // 10 bps
        p.tp1_price = 50100.0;  // 20 bps (2R)
        return p;
    }
};

TEST_F(RiskManagerTest, RejectsOnKillSwitch) {
    TickerUniverse universe;
    NewsCalendar news;
    RiskManager risk(universe, news);

    auto state = make_state();
    state.kill_switch_triggered = true;

    auto decision = risk.evaluate(make_plan(), state);
    EXPECT_FALSE(decision.accepted);
    EXPECT_EQ(decision.reason, RejectReason::KillSwitchActive);
}

TEST_F(RiskManagerTest, RejectsOnDailyLossLimit) {
    TickerUniverse universe;
    NewsCalendar news;
    RiskManager risk(universe, news);

    auto state = make_state();
    state.realized_pnl_today_usd = -400.0; // -4% (limit 3%)

    auto decision = risk.evaluate(make_plan(), state);
    EXPECT_FALSE(decision.accepted);
    EXPECT_EQ(decision.reason, RejectReason::DailyLossLimitHit);
}

TEST_F(RiskManagerTest, RejectsOnTooManyPositions) {
    TickerUniverse universe;
    NewsCalendar news;
    RiskManager risk(universe, news);

    auto state = make_state();
    state.active_positions = 3;

    auto decision = risk.evaluate(make_plan(), state);
    EXPECT_FALSE(decision.accepted);
    EXPECT_EQ(decision.reason, RejectReason::TooManyPositions);
}

TEST_F(RiskManagerTest, RejectsOnTightStop) {
    TickerUniverse universe;
    NewsCalendar news;
    RiskManager::Config cfg;
    cfg.whitelist_tickers = {"BTCUSDT"};
    RiskManager risk(universe, news, cfg);

    auto plan = make_plan();
    plan.ticker = "BTC_USDT";
    plan.stop_price = 49995.0; // 1 bps (min 3 bps)

    auto decision = risk.evaluate(plan, make_state());
    EXPECT_FALSE(decision.accepted);
    EXPECT_EQ(decision.reason, RejectReason::StopTooTight);
}

TEST_F(RiskManagerTest, CalculatesSizeCorrect_05PctRisk) {
    TickerUniverse universe;
    NewsCalendar news;
    RiskManager::Config cfg;
    cfg.max_position_value_pct = 500.0;
    cfg.whitelist_tickers = {"BTCUSDT"};
    RiskManager risk(universe, news, cfg);

    auto state = make_state();
    state.equity_usd = 100000.0;
    state.starting_equity_usd = 100000.0;
    state.free_balance_usd = 200000.0; // plenty of margin

    auto plan = make_plan(); // 10 bps stop at 50000
    plan.ticker = "BTC_USDT";

    // 0.5% risk of 100000 = 500 USD
    // 10 bps stop at 50000 is 50 USD/coin
    // size should be 10.0 coins (500k USD value, 5x leverage)

    auto decision = risk.evaluate(plan, state);
    EXPECT_TRUE(decision.accepted) << decision.details;
    EXPECT_NEAR(decision.adjusted_size_coin, 10.0, 0.001);
    EXPECT_NEAR(decision.risk_usd, 500.0, 0.001);
}

TEST_F(RiskManagerTest, AcceptsExactOneRByPriceDistanceLongAndShort) {
    TickerUniverse universe;
    NewsCalendar news;
    RiskManager::Config cfg;
    cfg.whitelist_tickers = {"BTCUSDT"};
    cfg.max_stop_bps = 6000.0;
    cfg.max_position_value_pct = 500.0;
    RiskManager risk(universe, news, cfg);
    auto state = make_state();
    state.free_balance_usd = 100000.0;

    auto long_plan = make_plan();
    long_plan.entry_price = 100.0;
    long_plan.stop_price = 50.0;
    long_plan.tp1_price = 150.0;
    auto long_decision = risk.evaluate(long_plan, state);
    EXPECT_TRUE(long_decision.accepted) << long_decision.details;

    auto short_plan = make_plan();
    short_plan.side = Side::Sell;
    short_plan.entry_price = 100.0;
    short_plan.stop_price = 150.0;
    short_plan.tp1_price = 50.0;
    auto short_decision = risk.evaluate(short_plan, state);
    EXPECT_TRUE(short_decision.accepted) << short_decision.details;
}

TEST_F(RiskManagerTest, RejectsTp1BelowOneRFinalGate) {
    TickerUniverse universe;
    NewsCalendar news;
    RiskManager::Config cfg;
    cfg.whitelist_tickers = {"BTCUSDT"};
    RiskManager risk(universe, news, cfg);
    AccountState state = make_state();
    state.free_balance_usd = 10000.0;
    TradePlan plan = make_plan();
    plan.tp1_price = 50025.0; // 5 bps < 1R stop distance

    auto decision = risk.evaluate(plan, state);
    EXPECT_FALSE(decision.accepted);
    EXPECT_EQ(decision.reason, RejectReason::PoorRewardRisk);
}
