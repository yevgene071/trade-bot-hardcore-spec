#include <gtest/gtest.h>
#include "transport/MarketDataFeed.hpp"
#include "logger/Logger.hpp"
#include <nlohmann/json.hpp>
#include <boost/core/ignore_unused.hpp>

using namespace trade_bot;

class MockWsClient : public IWsClient {
public:
    void connect(const std::string& url) override { boost::ignore_unused(url); m_connected = true; if (m_on_connect) m_on_connect(); }
    void send(std::string_view message) override { m_sent_messages.push_back(std::string(message)); }
    void disconnect() override { m_connected = false; }
    void set_on_message(std::function<void(const nlohmann::json&)> cb) override { m_on_message = cb; }
    void set_on_close(std::function<void(int, const std::string&)> cb) override { boost::ignore_unused(cb); }
    void set_on_error(std::function<void(const std::string&)> cb) override { boost::ignore_unused(cb); }
    void set_on_connect(std::function<void()> cb) override { m_on_connect = cb; }
    bool is_connected() const override { return m_connected; }

    void simulate_message(const nlohmann::json& j) { if (m_on_message) m_on_message(j); }

    std::vector<std::string> m_sent_messages;
    bool m_connected = false;
    std::function<void(const nlohmann::json&)> m_on_message;
    std::function<void()> m_on_connect;
};

class MockListener : public IMarketDataListener {
public:
    void on_trade(const Ticker& t, const Trade& tr) override { boost::ignore_unused(t); trades.push_back(tr); }
    void on_orderbook_snapshot(const OrderBookSnapshot& s) override { snapshots.push_back(s); }
    void on_orderbook_update(const OrderBookUpdate& u) override { updates.push_back(u); }
    void on_order_update(const OrderUpdate& u) override { boost::ignore_unused(u); }
    void on_position_update(const PositionUpdate& u) override { boost::ignore_unused(u); }
    void on_balance_update(const BalanceUpdate& u) override { boost::ignore_unused(u); }
    void on_error(const std::string& msg) override { boost::ignore_unused(msg); }

    std::vector<Trade> trades;
    std::vector<OrderBookSnapshot> snapshots;
    std::vector<OrderBookUpdate> updates;
};

class FeedTest : public ::testing::Test {
protected:
    void SetUp() override {
        trade_bot::Logger::init("test_logs/feed_test.log");
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
    EXPECT_EQ(listener.trades[0].price, 60000.0);
    EXPECT_EQ(listener.trades[0].side, Side::Buy);
}
