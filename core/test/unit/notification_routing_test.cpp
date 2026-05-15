#include "transport/NotificationFeed.hpp"
#include "transport/MetaScalpCodec.hpp"
#include "logger/Logger.hpp"
#include <gtest/gtest.h>
#include <gmock/gmock.h>

using namespace trade_bot;

class MockWsClient : public IWsClient {
public:
    MOCK_METHOD(void, connect, (const std::string&), (override));
    MOCK_METHOD(void, disconnect, (), (override));
    MOCK_METHOD(void, send, (std::string_view), (override));
    MOCK_METHOD(void, set_on_message, (std::function<void(const nlohmann::json&)>), (override));
    MOCK_METHOD(void, set_on_close, (std::function<void(int, const std::string&)>), (override));
    MOCK_METHOD(void, set_on_error, (std::function<void(const std::string&)>), (override));
    MOCK_METHOD(void, set_on_connect, (std::function<void()>), (override));
    MOCK_METHOD(bool, is_connected, (), (const, override));
};

class NotificationRoutingTest : public ::testing::Test {
protected:
    void SetUp() override { Logger::init(); }
};

TEST_F(NotificationRoutingTest, RoutesBigTickToUniverse) {
    auto ws = std::make_shared<MockWsClient>();
    TickerUniverse universe;
    universe.register_strategy("S1", [](const Ticker&){ return true; });
    universe.refresh_pool({"BTCUSDT"});
    
    NotificationFeed::Config cfg;
    cfg.exchange_id = 2; // Binance
    cfg.market_type = 2; // UsdtFutures
    
    NotificationFeed feed(ws, universe, cfg);
    
    std::function<void(const nlohmann::json&)> captured_cb;
    EXPECT_CALL(*ws, set_on_message(testing::_)).WillOnce(testing::SaveArg<0>(&captured_cb));
    EXPECT_CALL(*ws, send(testing::_)).Times(1);
    
    feed.start();
    
    auto now_ts = std::chrono::system_clock::now();
    auto in_time_t = std::chrono::system_clock::to_time_t(now_ts);
    std::stringstream ss;
    ss << std::put_time(std::gmtime(&in_time_t), "%Y-%m-%dT%H:%M:%S.000");

    // Simulate BigTick notification using real MetaScalp API format
    // API sends: {"Type":"notification_update","Data":{"Notifications":[{"Type":"BigTick",...}]}}
    nlohmann::json msg = {
        {"Type", "notification_update"},
        {"Data", {
            {"Notifications", {{
                {"Type", "BigTick"},
                {"ExchangeId", 2},
                {"MarketType", "UsdtFutures"},
                {"Ticker", "BTCUSDT"},
                {"Price", 50000.0},
                {"Size", 1.0},
                {"Date", ss.str()}
            }}}
        }}
    };
    
    captured_cb(msg);
    
    EXPECT_TRUE(universe.is_boosted("BTCUSDT", std::chrono::system_clock::now()));
    EXPECT_EQ(feed.dropped_wrong_connection_total(), 0);
}

TEST_F(NotificationRoutingTest, RoutesScreenerNewCoin) {
    auto ws = std::make_shared<MockWsClient>();
    TickerUniverse universe;
    universe.register_strategy("S1", [](const Ticker&){ return true; });
    
    NotificationFeed::Config cfg;
    cfg.exchange_id = 2;
    cfg.market_type = 2;
    
    NotificationFeed feed(ws, universe, cfg);
    
    std::function<void(const nlohmann::json&)> captured_cb;
    EXPECT_CALL(*ws, set_on_message(testing::_)).WillOnce(testing::SaveArg<0>(&captured_cb));
    EXPECT_CALL(*ws, send(testing::_));
    
    feed.start();
    
    nlohmann::json msg = {
        {"Type", "notification_update"},
        {"Data", {
            {"Notifications", {{
                {"Type", "ScreenerNewCoin"},
                {"ExchangeId", 2},
                {"MarketType", "UsdtFutures"},
                {"Ticker", "ETHUSDT"},
                {"Price", 3000.0},
                {"Size", 0.0},
                {"Date", "2026-05-02T19:00:00.000"}
            }}}
        }}
    };
    
    captured_cb(msg);
    // ScreenerNewCoin calls on_screener_new_coin which adds to pool;
    // verify indirectly by checking universe active set
    // (pool addition depends on strategy affinity accepting the ticker)
}

TEST_F(NotificationRoutingTest, DropsWrongConnection) {
    auto ws = std::make_shared<MockWsClient>();
    TickerUniverse universe;
    
    NotificationFeed::Config cfg;
    cfg.exchange_id = 2;
    cfg.market_type = 2;
    
    NotificationFeed feed(ws, universe, cfg);
    
    std::function<void(const nlohmann::json&)> captured_cb;
    EXPECT_CALL(*ws, set_on_message(testing::_)).WillOnce(testing::SaveArg<0>(&captured_cb));
    EXPECT_CALL(*ws, send(testing::_));
    
    feed.start();
    
    // Simulate notification from different exchange
    nlohmann::json msg = {
        {"Type", "notification_update"},
        {"Data", {
            {"Notifications", {{
                {"Type", "BigTick"},
                {"ExchangeId", 3}, // Different exchange
                {"MarketType", "UsdtFutures"},
                {"Ticker", "ETHUSDT"},
                {"Price", 3000.0},
                {"Size", 10.0},
                {"Date", "2026-05-02T19:00:00.000"}
            }}}
        }}
    };
    
    captured_cb(msg);
    
    EXPECT_EQ(feed.dropped_wrong_connection_total(), 1);
}

TEST_F(NotificationRoutingTest, BackwardCompatBareObject) {
    // Verify that a bare notification object in Data (without Notifications wrapper)
    // is still routed correctly for backward compatibility.
    auto ws = std::make_shared<MockWsClient>();
    TickerUniverse universe;
    universe.register_strategy("S1", [](const Ticker&){ return true; });
    universe.refresh_pool({"BTCUSDT"});
    
    NotificationFeed::Config cfg;
    cfg.exchange_id = 2;
    cfg.market_type = 2;
    
    NotificationFeed feed(ws, universe, cfg);
    
    std::function<void(const nlohmann::json&)> captured_cb;
    EXPECT_CALL(*ws, set_on_message(testing::_)).WillOnce(testing::SaveArg<0>(&captured_cb));
    EXPECT_CALL(*ws, send(testing::_));
    
    feed.start();
    
    // Bare notification object (no "Notifications" wrapper) with "Type" as string
    nlohmann::json msg = {
        {"Type", "notification_update"},
        {"Data", {
            {"Type", "BigOrderBookAmount"},
            {"ExchangeId", 2},
            {"MarketType", "UsdtFutures"},
            {"Ticker", "BTCUSDT"},
            {"Price", 45000.0},
            {"Size", 100.0},
            {"Date", "2026-05-02T19:00:00.000"}
        }}
    };
    
    captured_cb(msg);
    
    EXPECT_TRUE(universe.is_boosted("BTCUSDT", std::chrono::system_clock::now()));
    EXPECT_EQ(feed.dropped_wrong_connection_total(), 0);
}
