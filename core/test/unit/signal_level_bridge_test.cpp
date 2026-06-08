#include "signals/SignalLevelBridge.hpp"
#include "transport/SignalLevelGateway.hpp"
#include "signals/SignalBus.hpp"
#include "logger/Logger.hpp"
#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include <nlohmann/json.hpp>

using namespace trade_bot;

class MockSignalLevelGateway : public SignalLevelGateway {
public:
    MockSignalLevelGateway(IHttpClient& h) : SignalLevelGateway(h, "", 0) {}
    MOCK_METHOD(int64_t, create, (const Ticker&, double), ());
    MOCK_METHOD(void, remove, (int64_t), ());
};

class FakeHttpClient5 : public IHttpClient {
public:
    HttpResponse get(const std::string& url) override {
        last_get_url = url;
        return next_get;
    }
    HttpResponse post(const std::string& url, const std::string& body) override {
        last_post_url = url;
        last_post_body = body;
        return next_post;
    }
    HttpResponse put(const std::string&, const std::string&) override { return {200, "{}", {}}; }
    HttpResponse del(const std::string&) override { return {200, "{}", {}}; }
    void set_timeout_ms(int) override {}

    HttpResponse next_get{200, "{}", {}};
    HttpResponse next_post{200, "{}", {}};
    std::string last_get_url;
    std::string last_post_url;
    std::string last_post_body;
};

class SignalLevelBridgeTest : public ::testing::Test {
protected:
    void SetUp() override { Logger::init(); }
};

TEST_F(SignalLevelBridgeTest, CreatesServerLevelOnFormed) {
    FakeHttpClient5 http;
    MockSignalLevelGateway gateway(http);
    SignalBus bus;
    SignalLevelBridge bridge(gateway, bus);
    
    EXPECT_CALL(gateway, create(Ticker("BTC_USDT"), 50000.0)).WillOnce(testing::Return(123));
    bridge.on_level_formed("BTCUSDT", 50000.0, std::chrono::system_clock::now(), 50000.0);
}

TEST_F(SignalLevelBridgeTest, PublishesSignalOnTrigger) {
    FakeHttpClient5 http;
    MockSignalLevelGateway gateway(http);
    SignalBus bus;
    SignalLevelBridge bridge(gateway, bus);
    
    int signals = 0;
    bus.subscribe([&signals](const Signal& s){
        if (s.kind == SignalKind::LevelBreak) signals++;
    });
    
    bridge.on_server_trigger(123, "BTCUSDT", 50000.0, std::chrono::system_clock::now());
    EXPECT_EQ(signals, 1);
}

TEST_F(SignalLevelBridgeTest, GatewayCreateFallsBackToListWhenPostReturnsNoId) {
    FakeHttpClient5 http;
    http.next_post = {200, R"({"Status":"ok"})", {}};
    http.next_get = {200, R"({
        "ConnectionId": 1,
        "Ticker": "BTCUSDT",
        "Count": 1,
        "SignalLevels": [
            {
                "Id": 777,
                "ConnectionId": 1,
                "Ticker": "BTCUSDT",
                "Price": 50000.0,
                "IsTriggered": false,
                "TriggerTime": null,
                "TriggerRule": "GreaterThanEqual"
            }
        ]
    })", {}};

    SignalLevelGateway gateway(http, "http://localhost", 1);
    auto id = gateway.create("BTC_USDT", 50000.0);

    EXPECT_EQ(id, 777);
    EXPECT_NE(http.last_get_url.find("/api/connections/1/signal-levels?Ticker=BTCUSDT"), std::string::npos);
    const auto body = nlohmann::json::parse(http.last_post_body);
    EXPECT_EQ(body["Ticker"], "BTCUSDT");
    EXPECT_DOUBLE_EQ(body["Price"], 50000.0);
}
