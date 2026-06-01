#include <gtest/gtest.h>
#include "transport/OrderGateway.hpp"
#include "logger/Logger.hpp"
#include <nlohmann/json.hpp>
#include <boost/core/ignore_unused.hpp>

using namespace trade_bot;

class MockHttpClient : public IHttpClient {
public:
    HttpResponse get(const std::string& url) override { 
        boost::ignore_unused(url);
        return m_next_response; 
    }
    HttpResponse post(const std::string& url, const std::string& body) override { 
        boost::ignore_unused(url, body);
        return m_next_response; 
    }
    HttpResponse put(const std::string& url, const std::string& body) override { 
        boost::ignore_unused(url, body);
        return m_next_response; 
    }
    HttpResponse del(const std::string& url) override { 
        boost::ignore_unused(url);
        return m_next_response; 
    }
    void set_timeout_ms(int timeout_ms) override { boost::ignore_unused(timeout_ms); }

    HttpResponse m_next_response;
};

class OrderGatewayTest : public ::testing::Test {
protected:
    void SetUp() override {
        trade_bot::Logger::init("test_logs/gateway_test.log");
    }
};

TEST_F(OrderGatewayTest, GetBalance) {
    auto http = std::make_shared<MockHttpClient>();
    OrderGateway gateway(http);
    
    nlohmann::json balance_json = {
        {"ConnectionId", 1},
        {"Balances", {{{"Coin", "USDT"}, {"Total", 1000.0}, {"Free", 800.0}, {"Locked", 200.0}}}}
    };
    
    http->m_next_response = {200, balance_json.dump(), {}};
    
    auto balance = gateway.get_balance(1);
    ASSERT_EQ(balance.balances.size(), 1);
    EXPECT_EQ(balance.balances[0].coin, "USDT");
    EXPECT_EQ(balance.balances[0].total, 1000.0);
}

TEST_F(OrderGatewayTest, PlaceOrder) {
    auto http = std::make_shared<MockHttpClient>();
    OrderGateway gateway(http);
    
    nlohmann::json result_json = {
        {"Status", "ok"},
        {"ClientId", "ms_123"},
        {"ExecutionTimeMs", 50.5}
    };
    
    http->m_next_response = {200, result_json.dump(), {}};
    
    PlaceOrderRequest req{"BTCUSDT", Side::Buy, 60000.0, 0.1, OrderType::Limit, false, ""};
    auto result = gateway.place_order(1, req);
    
    EXPECT_EQ(result.status, "ok");
    EXPECT_EQ(result.client_id, "ms_123");
}

TEST_F(OrderGatewayTest, GetConnectionsWrapped) {
    auto http = std::make_shared<MockHttpClient>();
    OrderGateway gateway(http);
    
    nlohmann::json connections_json = {
        {"connections", {
            {{"Id", 1}, {"Name", "Test Connection"}, {"State", "Connected"}, {"ViewMode", false}}
        }}
    };
    
    http->m_next_response = {200, connections_json.dump(), {}};
    
    auto connections = gateway.get_connections();
    ASSERT_EQ(connections.size(), 1);
    EXPECT_EQ(connections[0].id, 1);
    EXPECT_EQ(connections[0].name, "Test Connection");
}

TEST_F(OrderGatewayTest, GetTickersWrapped) {
    auto http = std::make_shared<MockHttpClient>();
    OrderGateway gateway(http);
    
    nlohmann::json tickers_json = {
        {"connectionId", 1},
        {"tickers", {
            {{"Name", "BTCUSDT"}, {"BaseAsset", "BTC"}, {"QuoteAsset", "USDT"}}
        }}
    };
    
    http->m_next_response = {200, tickers_json.dump(), {}};
    
    auto tickers = gateway.get_tickers(1);
    ASSERT_EQ(tickers.size(), 1);
    EXPECT_EQ(tickers[0].name, "BTCUSDT");
}

