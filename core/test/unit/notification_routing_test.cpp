#include "transport/NotificationFeed.hpp"
#include "transport/MetaScalpCodec.hpp"
#include "transport/SignalLevelGateway.hpp"
#include "signals/SignalBus.hpp"
#include "signals/SignalLevelBridge.hpp"
#include "logger/Logger.hpp"
#include <gtest/gtest.h>
#include <gmock/gmock.h>

#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>

using namespace trade_bot;

namespace {

std::string current_meta_timestamp() {
    auto now_ts = std::chrono::system_clock::now();
    auto in_time_t = std::chrono::system_clock::to_time_t(now_ts);
    std::stringstream ss;
    ss << std::put_time(std::gmtime(&in_time_t), "%Y-%m-%dT%H:%M:%S.000");
    return ss.str();
}

}  // namespace

class MockWsClient : public IWsClient {
public:
    MOCK_METHOD(void, connect, (const std::string&), (override));
    MOCK_METHOD(void, disconnect, (), (override));
    MOCK_METHOD(void, send, (std::string_view), (override));
    MOCK_METHOD(void, set_on_message, (std::function<void(const nlohmann::json&, uint64_t, TraceId)>), (override));
    MOCK_METHOD(void, set_on_close, (std::function<void(int, const std::string&)>), (override));
    MOCK_METHOD(void, set_on_error, (std::function<void(const std::string&)>), (override));
    MOCK_METHOD(void, set_on_connect, (std::function<void()>), (override));
    MOCK_METHOD(bool, is_connected, (), (const, override));
};

class FakeHttpClient : public IHttpClient {
public:
    HttpResponse get(const std::string&) override { return {200, R"({"SignalLevels":[]})", {}}; }
    HttpResponse post(const std::string&, const std::string&) override { return {200, R"({"Status":"ok"})", {}}; }
    HttpResponse put(const std::string&, const std::string&) override { return {200, "{}", {}}; }
    HttpResponse del(const std::string&) override { return {200, "{}", {}}; }
    void set_timeout_ms(int) override {}
};

class NotificationRoutingTest : public ::testing::Test {
protected:
    void SetUp() override { Logger::init(); }
};

TEST_F(NotificationRoutingTest, RoutesBigTickToUniverse) {
    auto ws = std::make_shared<MockWsClient>();
    TickerUniverse universe;
    universe.register_strategy("S1", [](const Ticker&){ return true; });
    universe.refresh_pool({"BTCUSDT"}, std::chrono::system_clock::now());
    
    NotificationFeed::Config cfg;
    cfg.exchange_id = 2; // Binance
    cfg.market_type = 2; // UsdtFutures
    
    NotificationFeed feed(ws, universe, cfg);
    
    std::function<void(const nlohmann::json&, uint64_t, TraceId)> captured_cb;
    EXPECT_CALL(*ws, set_on_message(testing::_)).WillOnce(testing::SaveArg<0>(&captured_cb));
    EXPECT_CALL(*ws, send(testing::_)).Times(1);
    
    feed.start();
    
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
                {"Date", current_meta_timestamp()}
            }}}
        }}
    };
    
    captured_cb(msg, 0, TraceId{0});
    
    EXPECT_TRUE(universe.is_boosted("BTCUSDT", std::chrono::system_clock::now()));
    EXPECT_EQ(feed.dropped_wrong_connection_total(), 0);
}

TEST_F(NotificationRoutingTest, SubscribesAndRoutesDirectSignalLevelTrigger) {
    auto ws = std::make_shared<MockWsClient>();
    TickerUniverse universe;
    FakeHttpClient http;
    SignalBus bus;
    SignalLevelGateway gateway(http, "http://localhost", 1);
    SignalLevelBridge bridge(gateway, bus);

    std::vector<Signal> emitted;
    bus.subscribe([&](const Signal& s) { emitted.push_back(s); });

    NotificationFeed::Config cfg;
    cfg.subscribe = false;
    cfg.signal_levels_subscribe = true;
    NotificationFeed feed(ws, universe, cfg, nullptr, &bridge);

    std::function<void(const nlohmann::json&, uint64_t, TraceId)> captured_cb;
    std::vector<std::string> sent;
    EXPECT_CALL(*ws, set_on_message(testing::_)).WillOnce(testing::SaveArg<0>(&captured_cb));
    EXPECT_CALL(*ws, send(testing::_))
        .Times(1)
        .WillRepeatedly(testing::Invoke([&](std::string_view msg) {
            sent.emplace_back(msg);
        }));

    feed.start();

    bool found_signal_level_sub = false;
    for (const auto& raw : sent) {
        auto j = nlohmann::json::parse(raw);
        if (j["Type"] == "signal_level_subscribe") {
            found_signal_level_sub = true;
        }
    }
    EXPECT_TRUE(found_signal_level_sub);

    nlohmann::json msg = {
        {"Type", "signal_level_triggered"},
        {"Data", {
            {"Id", 777},
            {"ConnectionId", 1},
            {"Ticker", "BTCUSDT"},
            {"Price", 50000.0},
            {"IsTriggered", true},
            {"TriggerTime", "2026-04-13T10:30:00+00:00"},
            {"TriggerRule", "GreaterThanEqual"}
        }}
    };
    captured_cb(msg, 0, TraceId{0});

    ASSERT_EQ(emitted.size(), 1u);
    EXPECT_EQ(emitted[0].kind, SignalKind::LevelBreak);
    EXPECT_EQ(emitted[0].ticker, "BTC_USDT");
    EXPECT_DOUBLE_EQ(emitted[0].price, 50000.0);
}

TEST_F(NotificationRoutingTest, RoutesScreenerNewCoin) {
    auto ws = std::make_shared<MockWsClient>();
    TickerUniverse universe;
    universe.register_strategy("S1", [](const Ticker&){ return true; });
    
    NotificationFeed::Config cfg;
    cfg.exchange_id = 2;
    cfg.market_type = 2;
    
    NotificationFeed feed(ws, universe, cfg);
    
    std::function<void(const nlohmann::json&, uint64_t, TraceId)> captured_cb;
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
    
    captured_cb(msg, 0, TraceId{0});
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
    
    std::function<void(const nlohmann::json&, uint64_t, TraceId)> captured_cb;
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
    
    captured_cb(msg, 0, TraceId{0});
    
    EXPECT_EQ(feed.dropped_wrong_connection_total(), 1);
}

TEST_F(NotificationRoutingTest, BackwardCompatBareObject) {
    // Verify that a bare notification object in Data (without Notifications wrapper)
    // is still routed correctly for backward compatibility.
    auto ws = std::make_shared<MockWsClient>();
    TickerUniverse universe;
    universe.register_strategy("S1", [](const Ticker&){ return true; });
    universe.refresh_pool({"BTCUSDT"}, std::chrono::system_clock::now());
    
    NotificationFeed::Config cfg;
    cfg.exchange_id = 2;
    cfg.market_type = 2;
    
    NotificationFeed feed(ws, universe, cfg);
    
    std::function<void(const nlohmann::json&, uint64_t, TraceId)> captured_cb;
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
            {"Date", current_meta_timestamp()}
        }}
    };
    
    captured_cb(msg, 0, TraceId{0});
    
    EXPECT_TRUE(universe.is_boosted("BTCUSDT", std::chrono::system_clock::now()));
    EXPECT_EQ(feed.dropped_wrong_connection_total(), 0);
}
