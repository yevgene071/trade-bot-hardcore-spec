#include "signals/SignalLevelBridge.hpp"
#include "transport/SignalLevelGateway.hpp"
#include "signals/SignalBus.hpp"
#include "logger/Logger.hpp"
#include <gtest/gtest.h>
#include <gmock/gmock.h>

using namespace trade_bot;

class MockSignalLevelGateway : public SignalLevelGateway {
public:
    MockSignalLevelGateway(IHttpClient& h) : SignalLevelGateway(h, "", 0) {}
    MOCK_METHOD(int, create, (const Ticker&, double), ());
    MOCK_METHOD(void, remove, (int), ());
};

class FakeHttpClient5 : public IHttpClient {
public:
    HttpResponse get(const std::string&) override { return {200, "{}", {}}; }
    HttpResponse post(const std::string&, const std::string&) override { return {200, "{}", {}}; }
    HttpResponse put(const std::string&, const std::string&) override { return {200, "{}", {}}; }
    HttpResponse del(const std::string&) override { return {200, "{}", {}}; }
    void set_timeout_ms(int) override {}
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
    
    EXPECT_CALL(gateway, create(Ticker("BTCUSDT"), 50000.0)).WillOnce(testing::Return(123));
    bridge.on_level_formed("BTCUSDT", 50000.0);
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
    
    bridge.on_server_trigger(123, "BTCUSDT", 50000.0);
    EXPECT_EQ(signals, 1);
}
