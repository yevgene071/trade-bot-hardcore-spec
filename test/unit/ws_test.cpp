#include <gtest/gtest.h>
#include "transport/BeastWsClient.hpp"
#include "logger/Logger.hpp"
#include <boost/asio.hpp>
#include <thread>

using namespace trade_bot;
namespace net = boost::asio;

class WsTest : public ::testing::Test {
protected:
    void SetUp() override {
        trade_bot::Logger::init("test_logs/ws_test.log");
    }
};

TEST_F(WsTest, UrlParsing) {
    net::io_context ioc;
    auto client = std::make_shared<BeastWsClient>(ioc);
    
    // We can't easily test private members, but we can verify it doesn't throw on connect call
    EXPECT_NO_THROW(client->connect("ws://127.0.0.1:17845/"));
}

// A more involved test would involve a mock server, similar to HttpClientTest but for WS.
