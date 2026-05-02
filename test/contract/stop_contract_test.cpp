#include "transport/OrderGateway.hpp"
#include "transport/CurlHttpClient.hpp"
#include "transport/MetaScalpDiscovery.hpp"
#include "transport/MarketDataFeed.hpp"
#include "transport/BeastWsClient.hpp"
#include "logger/Logger.hpp"
#include <gtest/gtest.h>
#include <thread>
#include <atomic>
#include <fstream>
#include <nlohmann/json.hpp>

using namespace trade_bot;

class StopContractTest : public ::testing::Test {
protected:
    void SetUp() override {
        char* run_contract = std::getenv("WITH_METASCALP");
        if (!run_contract) {
            GTEST_SKIP() << "Skipping contract test: WITH_METASCALP env var not set";
        }

        Logger::init();
        
        auto port = MetaScalpDiscovery{std::make_shared<CurlHttpClient>()}.discover();
        ASSERT_TRUE(port.has_value()) << "MetaScalp not found";
        
        auto http = std::make_shared<CurlHttpClient>();
        gateway = std::make_unique<OrderGateway>(http);
        gateway->set_port(*port);
        
        auto connections = gateway->get_connections();
        for (const auto& conn : connections) {
            if (conn.state == "Connected" && !conn.view_mode) {
                connection_id = conn.id;
                break;
            }
        }
        ASSERT_NE(connection_id, -1) << "No suitable trading connection found";
    }

    std::unique_ptr<OrderGateway> gateway;
    int connection_id = -1;
    Ticker ticker = "BTCUSDT"; // Use BTCUSDT as default for contract test
};

struct TestResult {
    bool stop_ok = false;
    bool stop_loss_ok = false;
    bool take_profit_ok = false;
};

void save_results(const TestResult& res) {
    nlohmann::json j;
    j["stop_ok"] = res.stop_ok;
    j["stop_loss_ok"] = res.stop_loss_ok;
    j["take_profit_ok"] = res.take_profit_ok;
    j["last_run"] = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    
    std::ofstream f("config/contract_test_results.json");
    f << j.dump(4);
}

TEST_F(StopContractTest, VerifyServerStops) {
    TestResult res;
    
    // 1. Verify Stop
    try {
        PlaceOrderRequest req;
        req.ticker = ticker;
        req.side = Side::Buy;
        req.price = 10000.0; // Very low price for stop buy (to be sure it's a stop)
        req.size = 0.001;
        req.type = OrderType::Stop;
        
        auto p_res = gateway->place_order(connection_id, req);
        LOG_INFO("Placed Stop order: status={}", p_res.status);
        
        auto orders = gateway->get_open_orders(connection_id, ticker);
        for (const auto& o : orders) {
            if (o.type == OrderType::Stop) {
                if (o.trigger_price && *o.trigger_price == req.price) {
                    res.stop_ok = true;
                } else if (o.price == req.price) {
                    // Some APIs might use price as trigger_price for stops
                    res.stop_ok = true;
                }
            }
        }
        gateway->cancel_all_orders(connection_id, ticker);
    } catch (const std::exception& e) {
        LOG_ERROR("Stop verification failed: {}", e.what());
    }

    // 2. Verify StopLoss
    try {
        PlaceOrderRequest req;
        req.ticker = ticker;
        req.side = Side::Sell;
        req.price = 10000.0;
        req.size = 0.001;
        req.type = OrderType::StopLoss;
        req.reduce_only = true;
        
        gateway->place_order(connection_id, req);
        auto orders = gateway->get_open_orders(connection_id, ticker);
        for (const auto& o : orders) {
            if (o.type == OrderType::StopLoss && o.price == req.price) {
                res.stop_loss_ok = true;
            }
        }
        gateway->cancel_all_orders(connection_id, ticker);
    } catch (const std::exception& e) {
        LOG_ERROR("StopLoss verification failed: {}", e.what());
    }

    // 3. Verify TakeProfit
    try {
        PlaceOrderRequest req;
        req.ticker = ticker;
        req.side = Side::Sell;
        req.price = 100000.0;
        req.size = 0.001;
        req.type = OrderType::TakeProfit;
        req.reduce_only = true;
        
        gateway->place_order(connection_id, req);
        auto orders = gateway->get_open_orders(connection_id, ticker);
        for (const auto& o : orders) {
            if (o.type == OrderType::TakeProfit && o.price == req.price) {
                res.take_profit_ok = true;
            }
        }
        gateway->cancel_all_orders(connection_id, ticker);
    } catch (const std::exception& e) {
        LOG_ERROR("TakeProfit verification failed: {}", e.what());
    }

    save_results(res);
    
    EXPECT_TRUE(res.stop_ok);
    EXPECT_TRUE(res.stop_loss_ok);
    EXPECT_TRUE(res.take_profit_ok);
}
