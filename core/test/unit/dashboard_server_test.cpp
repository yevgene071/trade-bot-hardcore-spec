#include "control/DashboardServer.hpp"
#include "logger/Logger.hpp"

#include <boost/asio/io_context.hpp>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <thread>

using namespace trade_bot;

namespace {

class DashboardServerTest : public ::testing::Test {
protected:
    void SetUp() override { Logger::init(); }

    // Find a free TCP port; tests run in parallel via ctest -j.
    static uint16_t pick_port() {
        boost::asio::io_context ioc;
        boost::asio::ip::tcp::acceptor acc(
            ioc, {boost::asio::ip::make_address("127.0.0.1"), 0});
        return acc.local_endpoint().port();
    }
};

DashboardServer::State make_state(double equity = 12345.67) {
    DashboardServer::State s;
    s.version       = "0.0.1";
    s.kill_switch_active = false;
    s.account.equity_usd            = equity;
    s.account.realized_pnl_today_usd = -50.0;
    s.account.unrealized_pnl_usd    = 12.5;
    s.account.free_balance_usd      = 11000.0;
    s.account.starting_equity_usd   = 10000.0;
    s.signal_counts["TapeBurst"]    = 7;

    // New fields
    s.server_time_unix = 1716500000;
    s.recorder_active = true;
    s.recorder_path = "/dumps/test.ndjson";

    s.risk.margin_used_pct = 12.3;
    s.risk.exposure_pct = 5.0;
    s.risk.total_trades_today = 8;
    s.risk.consecutive_losses = 2;
    s.risk.daily_pnl_pct = -0.5;
    s.risk.current_drawdown_pct = -1.2;

    {
        DashboardServer::State::StrategyStats st;
        st.name = "BounceFromDensity";
        st.total_trades = 12;
        st.wins = 8;
        st.losses = 4;
        st.total_pnl = 150.0;
        st.best_pnl = 45.0;
        st.worst_pnl = -20.0;
        s.strategy_stats.push_back(st);
    }

    {
        DashboardServer::State::FundingInfo fi;
        fi.ticker = "BTC_USDT";
        fi.rate = 0.0001;
        fi.next_funding_unix = 1716510000;
        s.funding_info.push_back(fi);
    }

    // DS-26: Strategy states
    {
        StrategyState ss;
        ss.ticker = "BTCUSDT";
        ss.strategy_name = "BreakoutEatThrough";
        ss.ready_state = StrategyReadyState::Warming;
        ss.readiness_pct = 62.5;
        ss.signals_last_60s = 3;
        ss.last_reject_reason = "Leader corr below 0.60";
        ss.seconds_since_last_reject = 12.0;
        {
            StrategyCondition c;
            c.name = "DensityEating";
            c.current = 1.0;
            c.target = 1.0;
            c.met = true;
            c.unit = "count";
            ss.conditions.push_back(c);
        }
        {
            StrategyCondition c;
            c.name = "Tape aggression";
            c.current = 0.15;
            c.target = 0.3;
            c.met = false;
            c.unit = "";
            ss.conditions.push_back(c);
        }
        s.strategy_states.push_back(ss);
    }

    // DS-26: Chart history
    for (int i = 0; i < 5; ++i) {
        ChartPoint pt;
        pt.ts_unix_ms = 1716500000 + i * 1000;
        pt.mid = 100.0 + i * 0.5;
        pt.best_bid = 99.9 + i * 0.5;
        pt.best_ask = 100.1 + i * 0.5;
        pt.spread_bps = 1.5;
        pt.buy_vol_5s = 10.0;
        pt.sell_vol_5s = 5.0;
        pt.volatility_1min_bps = 3.0;
        pt.tape_aggression = 0.25;
        pt.leader_change_1s = 0.001;
        pt.leader_correlation = 0.7;
        s.chart_history.push_back(pt);
    }

    // DS-26: Order book levels
    s.bids_top20.push_back({99.99, 1.5});
    s.bids_top20.push_back({99.98, 2.0});
    s.asks_top20.push_back({100.01, 1.0});
    s.asks_top20.push_back({100.02, 2.5});
    s.ob_mid = 100.0;
    s.ob_spread_bps = 1.5;
    s.ob_imbalance = 0.33;
    s.selected_ticker = "BTC_USDT";

    return s;
}

}  // namespace

// #120 — update_state and serialize_state must NOT deadlock when called
// from the same thread that previously held the lock. Pre-fix, this would
// deadlock if a Session was registered and update_state called send_update
// re-entering the mutex.
TEST_F(DashboardServerTest, UpdateStateAndSerializeAreReentrancySafe) {
    boost::asio::io_context ioc;
    DashboardServer ds(ioc, "127.0.0.1", pick_port());

    std::atomic<bool> done{false};
    std::thread t([&] {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
        for (int i = 0; i < 1000; ++i) {
            ds.update_state(make_state(1000.0 + i));
            (void)ds.serialize_state();
            if (std::chrono::steady_clock::now() > deadline) break;
        }
        done.store(true);
    });
    t.join();
    EXPECT_TRUE(done.load()) << "broadcaster looked stuck — deadlock regression";
}

TEST_F(DashboardServerTest, SerializeStatePopulatesAllSections) {
    boost::asio::io_context ioc;
    DashboardServer ds(ioc, "127.0.0.1", pick_port());
    ds.update_state(make_state());

    auto j = nlohmann::json::parse(ds.serialize_state());
    EXPECT_EQ(j["version"],            "0.0.1");
    EXPECT_DOUBLE_EQ(j["account"]["equity_usd"].get<double>(),   12345.67);
    EXPECT_DOUBLE_EQ(j["account"]["realized_pnl_today_usd"].get<double>(), -50.0);
    EXPECT_FALSE(j["kill_switch_active"].get<bool>());
    EXPECT_EQ(j["signal_counts"]["TapeBurst"].get<int>(),        7);
    EXPECT_TRUE(j["open_trades"].is_array());
    EXPECT_TRUE(j["recent_journal"].is_array());
}

// #121 — auth check. Empty token = auth disabled (loopback fallback).
TEST_F(DashboardServerTest, AuthorizeWithoutTokenAcceptsAnything) {
    boost::asio::io_context ioc;
    DashboardServer ds(ioc, "127.0.0.1", pick_port(), /*auth_token=*/"");
    EXPECT_TRUE(ds.authorize(""));
    EXPECT_TRUE(ds.authorize("Bearer anything"));
}

TEST_F(DashboardServerTest, AuthorizeWithTokenRejectsMissingHeader) {
    boost::asio::io_context ioc;
    DashboardServer ds(ioc, "127.0.0.1", pick_port(), /*auth_token=*/"super-secret");
    EXPECT_FALSE(ds.authorize(""));
    EXPECT_FALSE(ds.authorize("Bearer"));            // no token
    EXPECT_FALSE(ds.authorize("Basic super-secret")); // wrong scheme
}

TEST_F(DashboardServerTest, AuthorizeWithTokenAcceptsExactMatchOnly) {
    boost::asio::io_context ioc;
    DashboardServer ds(ioc, "127.0.0.1", pick_port(), /*auth_token=*/"super-secret");
    EXPECT_TRUE (ds.authorize("Bearer super-secret"));
    EXPECT_FALSE(ds.authorize("Bearer super-secret-extra"));   // length differs
    EXPECT_FALSE(ds.authorize("Bearer super-secre"));          // length differs
    EXPECT_FALSE(ds.authorize("Bearer SUPER-SECRET"));         // case-sensitive
    EXPECT_FALSE(ds.authorize("Bearer "));
}

TEST_F(DashboardServerTest, SerializeNewStateFields) {
    boost::asio::io_context ioc;
    DashboardServer ds(ioc, "127.0.0.1", pick_port());
    ds.update_state(make_state());

    auto j = nlohmann::json::parse(ds.serialize_state());
    EXPECT_EQ(j["server_time_unix"].get<int64_t>(), 1716500000);
    EXPECT_TRUE(j["recorder_active"].get<bool>());
    EXPECT_EQ(j["recorder_path"].get<std::string>(), "/dumps/test.ndjson");

    // Risk snapshot
    EXPECT_DOUBLE_EQ(j["risk"]["margin_used_pct"].get<double>(), 12.3);
    EXPECT_DOUBLE_EQ(j["risk"]["exposure_pct"].get<double>(), 5.0);
    EXPECT_EQ(j["risk"]["total_trades_today"].get<int>(), 8);
    EXPECT_EQ(j["risk"]["consecutive_losses"].get<int>(), 2);
    EXPECT_DOUBLE_EQ(j["risk"]["daily_pnl_pct"].get<double>(), -0.5);
    EXPECT_DOUBLE_EQ(j["risk"]["current_drawdown_pct"].get<double>(), -1.2);

    // Strategy stats
    ASSERT_EQ(j["strategy_stats"].size(), 1u);
    EXPECT_EQ(j["strategy_stats"][0]["name"].get<std::string>(), "BounceFromDensity");
    EXPECT_EQ(j["strategy_stats"][0]["total_trades"].get<int>(), 12);
    EXPECT_EQ(j["strategy_stats"][0]["wins"].get<int>(), 8);
    EXPECT_EQ(j["strategy_stats"][0]["losses"].get<int>(), 4);
    EXPECT_DOUBLE_EQ(j["strategy_stats"][0]["total_pnl"].get<double>(), 150.0);

    // Funding info
    ASSERT_EQ(j["funding_info"].size(), 1u);
    EXPECT_EQ(j["funding_info"][0]["ticker"].get<std::string>(), "BTC_USDT");
    EXPECT_DOUBLE_EQ(j["funding_info"][0]["rate"].get<double>(), 0.0001);
    EXPECT_EQ(j["funding_info"][0]["next_funding_unix"].get<int64_t>(), 1716510000);
}

TEST_F(DashboardServerTest, NoSessionsAtConstruction) {
    boost::asio::io_context ioc;
    DashboardServer ds(ioc, "127.0.0.1", pick_port());
    EXPECT_EQ(ds.session_count(), 0u);
}

// DS-26: Verify new dashboard fields are serialized in JSON
TEST_F(DashboardServerTest, SerializeNewDashboardFields) {
    boost::asio::io_context ioc;
    DashboardServer ds(ioc, "127.0.0.1", pick_port());
    ds.update_state(make_state());

    auto j = nlohmann::json::parse(ds.serialize_state());

    // strategy_states
    ASSERT_TRUE(j.contains("strategy_states"));
    ASSERT_EQ(j["strategy_states"].size(), 1u);
    EXPECT_EQ(j["strategy_states"][0]["ticker"].get<std::string>(), "BTCUSDT");
    EXPECT_EQ(j["strategy_states"][0]["strategy_name"].get<std::string>(), "BreakoutEatThrough");
    EXPECT_EQ(j["strategy_states"][0]["ready_state"].get<int>(), static_cast<int>(StrategyReadyState::Warming));
    EXPECT_DOUBLE_EQ(j["strategy_states"][0]["readiness_pct"].get<double>(), 62.5);
    EXPECT_EQ(j["strategy_states"][0]["signals_last_60s"].get<int>(), 3);
    EXPECT_EQ(j["strategy_states"][0]["last_reject_reason"].get<std::string>(), "Leader corr below 0.60");
    EXPECT_DOUBLE_EQ(j["strategy_states"][0]["seconds_since_last_reject"].get<double>(), 12.0);

    // Conditions inside strategy_states
    ASSERT_EQ(j["strategy_states"][0]["conditions"].size(), 2u);
    EXPECT_EQ(j["strategy_states"][0]["conditions"][0]["name"].get<std::string>(), "DensityEating");
    EXPECT_TRUE(j["strategy_states"][0]["conditions"][0]["met"].get<bool>());
    EXPECT_FALSE(j["strategy_states"][0]["conditions"][1]["met"].get<bool>());

    // chart_history
    ASSERT_TRUE(j.contains("chart_history"));
    ASSERT_EQ(j["chart_history"].size(), 5u);
    EXPECT_DOUBLE_EQ(j["chart_history"][0]["mid"].get<double>(), 100.0);
    EXPECT_DOUBLE_EQ(j["chart_history"][4]["mid"].get<double>(), 102.0);
    EXPECT_EQ(j["chart_history"][0]["ts"].get<int64_t>(), 1716500000);
    EXPECT_DOUBLE_EQ(j["chart_history"][2]["tape_aggression"].get<double>(), 0.25);
    EXPECT_DOUBLE_EQ(j["chart_history"][2]["leader_correlation"].get<double>(), 0.7);

    // bids_top20 / asks_top20
    ASSERT_TRUE(j.contains("bids_top20"));
    ASSERT_EQ(j["bids_top20"].size(), 2u);
    EXPECT_DOUBLE_EQ(j["bids_top20"][0]["price"].get<double>(), 99.99);
    EXPECT_DOUBLE_EQ(j["bids_top20"][0]["size"].get<double>(), 1.5);

    ASSERT_TRUE(j.contains("asks_top20"));
    ASSERT_EQ(j["asks_top20"].size(), 2u);
    EXPECT_DOUBLE_EQ(j["asks_top20"][1]["price"].get<double>(), 100.02);
    EXPECT_DOUBLE_EQ(j["asks_top20"][1]["size"].get<double>(), 2.5);

    // ob_mid, ob_spread_bps, ob_imbalance
    EXPECT_DOUBLE_EQ(j["ob_mid"].get<double>(), 100.0);
    EXPECT_DOUBLE_EQ(j["ob_spread_bps"].get<double>(), 1.5);
    EXPECT_DOUBLE_EQ(j["ob_imbalance"].get<double>(), 0.33);

    // selected_ticker
    EXPECT_EQ(j["selected_ticker"].get<std::string>(), "BTC_USDT");
}
