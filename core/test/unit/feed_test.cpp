#include <gtest/gtest.h>
#include "transport/MarketDataFeed.hpp"
#include "config/Config.hpp"
#include "logger/Logger.hpp"
#include <nlohmann/json.hpp>
#include <boost/core/ignore_unused.hpp>

#include <cstdio>
#include <fstream>
#include <stdexcept>

using namespace trade_bot;

class MockWsClient : public IWsClient {
public:
    void connect(const std::string& url) override { boost::ignore_unused(url); m_connected = true; if (m_on_connect) m_on_connect(); }
    void send(std::string_view message) override { m_sent_messages.push_back(std::string(message)); }
    void disconnect() override { m_connected = false; }
    void set_on_message(std::function<void(const nlohmann::json&, uint64_t, TraceId)> cb) override { m_on_message = cb; }
    void set_on_close(std::function<void(int, const std::string&)> cb) override { boost::ignore_unused(cb); }
    void set_on_error(std::function<void(const std::string&)> cb) override { boost::ignore_unused(cb); }
    void set_on_connect(std::function<void()> cb) override { m_on_connect = cb; }
    bool is_connected() const override { return m_connected; }

    void simulate_message(const nlohmann::json& j) { if (m_on_message) m_on_message(j, 0, 0); }

    std::vector<std::string> m_sent_messages;
    bool m_connected = false;
    std::function<void(const nlohmann::json&, uint64_t, TraceId)> m_on_message;
    std::function<void()> m_on_connect;
};

class MockListener : public IMarketDataListener {
public:
    void on_trade(const Ticker& t, const Trade& tr) override { tickers.push_back(t); trades.push_back(tr); }
    void on_orderbook_snapshot(const OrderBookSnapshot& s) override { snapshots.push_back(s); }
    void on_orderbook_update(const OrderBookUpdate& u) override { updates.push_back(u); }
    void on_order_update(const OrderUpdate& u) override { boost::ignore_unused(u); }
    void on_position_update(const PositionUpdate& u) override { boost::ignore_unused(u); }
    void on_balance_update(const BalanceUpdate& u) override { boost::ignore_unused(u); }
    void on_finres_update(const FinresUpdate& u) override { boost::ignore_unused(u); }
    void on_error(const std::string& msg) override { boost::ignore_unused(msg); }

    std::vector<Ticker> tickers;
    std::vector<Trade> trades;
    std::vector<OrderBookSnapshot> snapshots;
    std::vector<OrderBookUpdate> updates;
};

class FeedTest : public ::testing::Test {
protected:
    void SetUp() override {
        trade_bot::Logger::init("test_logs/feed_test.log");
    }

    void load_feed_config(bool fetch_snapshot_on_subscribe) {
        std::ofstream cfg("feed_test_config.toml");
        cfg << R"(
[app]
version = "0.0.1"
name = "test"
[logger]
level = "info"
path = "test_logs/feed_test.log"
[network]
http_timeout_ms = 5000
[risk]
max_daily_loss_pct = 3.0
max_per_trade_risk_pct = 0.5
[clock]
sources = ["pool.ntp.org"]
[feed]
depth_levels = 50
depth_percent = 0.5
)";
        cfg << "fetch_snapshot_on_subscribe = "
            << (fetch_snapshot_on_subscribe ? "true" : "false") << "\n";
        cfg.close();
        Config::load("feed_test_config.toml");
        std::remove("feed_test_config.toml");
    }
};

TEST_F(FeedTest, Subscriptions) {
    auto ws = std::make_shared<MockWsClient>();
    MarketDataFeed feed(ws, 1);
    
    feed.subscribe_ticker("BTCUSDT");
    
    // Not connected yet, but should send when connected or if connect() called
    ws->connect("ws://127.0.0.1");
    feed.start();

    // Check sent messages (re-subscription + direct subscription)
    bool found_trade_sub = false;
    for (const auto& msg : ws->m_sent_messages) {
        auto j = nlohmann::json::parse(msg);
        if (j["Type"] == "trade_subscribe" && j["Data"]["Ticker"] == "BTCUSDT") found_trade_sub = true;
    }
    EXPECT_TRUE(found_trade_sub);
}

TEST_F(FeedTest, EventDistribution) {
    auto ws = std::make_shared<MockWsClient>();
    MarketDataFeed feed(ws, 1);
    MockListener listener;
    feed.add_listener(&listener);
    
    nlohmann::json trade_msg = {
        {"Type", "trade_update"},
        {"Data", {
            {"Ticker", "BTCUSDT"},
            {"Trades", {{{"Price", 60000.0}, {"Size", 0.5}, {"Side", "Buy"}, {"Time", "2024-05-01T12:00:00.000Z"}}}}
        }}
    };
    
    ws->simulate_message(trade_msg);
    
    ASSERT_EQ(listener.trades.size(), 1);
    ASSERT_EQ(listener.tickers.size(), 1);
    EXPECT_EQ(listener.tickers[0], "BTC_USDT");
    EXPECT_EQ(listener.trades[0].price, 60000.0);
    EXPECT_EQ(listener.trades[0].side, Side::Buy);
}

TEST_F(FeedTest, SpecificListenerNormalizesTickerKeys) {
    auto ws = std::make_shared<MockWsClient>();
    MarketDataFeed feed(ws, 1);
    MockListener listener;
    feed.add_listener("BTCUSDT", &listener);

    nlohmann::json trade_msg = {
        {"Type", "trade_update"},
        {"Data", {
            {"Ticker", "BTCUSDT"},
            {"Trades", {{{"Price", 60000.0}, {"Size", 0.5}, {"Side", "Buy"}, {"Time", "2024-05-01T12:00:00.000Z"}}}}
        }}
    };

    ws->simulate_message(trade_msg);

    ASSERT_EQ(listener.trades.size(), 1);
    ASSERT_EQ(listener.tickers.size(), 1);
    EXPECT_EQ(listener.tickers[0], "BTC_USDT");
}

TEST_F(FeedTest, RestSeedSendsFetchSnapshotFalseWhenEnabled) {
    load_feed_config(false);
    auto ws = std::make_shared<MockWsClient>();
    MarketDataFeed feed(ws, 1);
    MockListener listener;
    feed.add_listener("BTCUSDT", &listener);

    int fetch_calls = 0;
    feed.set_orderbook_snapshot_fetcher([&](const Ticker& ticker,
                                            int zoom_index,
                                            std::optional<int> depth_levels,
                                            std::optional<double> depth_percent) -> OrderBookSnapshot {
        ++fetch_calls;
        EXPECT_EQ(ticker, "BTC_USDT");
        EXPECT_EQ(zoom_index, 0);
        EXPECT_TRUE(depth_levels.has_value());
        if (depth_levels) {
            EXPECT_EQ(*depth_levels, 50);
        }
        EXPECT_TRUE(depth_percent.has_value());
        if (depth_percent) {
            EXPECT_DOUBLE_EQ(*depth_percent, 0.5);
        }
        return OrderBookSnapshot{
            .ticker = "BTC_USDT",
            .asks = {{101.0, 1.0, Side::Sell}},
            .bids = {{100.0, 1.0, Side::Buy}},
            .ts = std::chrono::system_clock::now()
        };
    });

    feed.subscribe_ticker("BTCUSDT");

    EXPECT_EQ(fetch_calls, 1);
    ASSERT_EQ(listener.snapshots.size(), 1u);
    EXPECT_EQ(listener.snapshots[0].ticker, "BTC_USDT");

    bool found_fetch_false = false;
    for (const auto& msg : ws->m_sent_messages) {
        auto j = nlohmann::json::parse(msg);
        if (j["Type"] == "orderbook_subscribe") {
            found_fetch_false = j["Data"].contains("FetchSnapshot") &&
                                j["Data"]["FetchSnapshot"].get<bool>() == false;
        }
    }
    EXPECT_TRUE(found_fetch_false);
}

TEST_F(FeedTest, RestSeed501FallsBackToWsSnapshotAndRemembersTicker) {
    load_feed_config(false);
    auto ws = std::make_shared<MockWsClient>();
    MarketDataFeed feed(ws, 1);

    int fetch_calls = 0;
    feed.set_orderbook_snapshot_fetcher([&](const Ticker&,
                                            int,
                                            std::optional<int>,
                                            std::optional<double>) -> OrderBookSnapshot {
        ++fetch_calls;
        throw std::runtime_error("Failed to get orderbook snapshot: status 501");
    });

    feed.subscribe_ticker("BTCUSDT");
    ws->m_sent_messages.clear();
    feed.subscribe_ticker("BTCUSDT");

    EXPECT_EQ(fetch_calls, 1);
    for (const auto& msg : ws->m_sent_messages) {
        auto j = nlohmann::json::parse(msg);
        if (j["Type"] == "orderbook_subscribe") {
            EXPECT_FALSE(j["Data"].contains("FetchSnapshot"));
        }
    }
}
