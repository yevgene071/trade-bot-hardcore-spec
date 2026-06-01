#include "executor/LiveExecutor.hpp"
#include "universe/TickerUniverse.hpp"
#include "logger/Logger.hpp"
#include <gtest/gtest.h>
#include <gmock/gmock.h>

using namespace trade_bot;
using namespace testing;

class MockOrderGatewayForExecutor : public IOrderGateway {
public:
    MOCK_METHOD(std::vector<ConnectionInfo>, get_connections, (), (override));
    MOCK_METHOD(ConnectionInfo, get_connection, (int), (override));
    MOCK_METHOD(std::vector<TickerInfo>, get_tickers, (int, bool), (override));
    MOCK_METHOD(std::vector<RestOrder>, get_open_orders, (int, const Ticker&), (override));
    MOCK_METHOD(std::vector<PositionUpdate>, get_positions, (int), (override));
    MOCK_METHOD(BalanceUpdate, get_balance, (int), (override));
    MOCK_METHOD(PlaceOrderResult, place_order, (int, const PlaceOrderRequest&), (override));
    MOCK_METHOD(void, cancel_order, (int, int64_t, const Ticker&), (override));
    MOCK_METHOD(void, cancel_all_orders, (int, const Ticker&), (override));
};

class DummyWsClient : public IWsClient {
public:
    void connect(const std::string&) override {}
    void send(std::string_view) override {}
    void disconnect() override {}
    void set_on_message(std::function<void(const nlohmann::json&, uint64_t, TraceId)>) override {}
    void set_on_close(std::function<void(int, const std::string&)>) override {}
    void set_on_error(std::function<void(const std::string&)>) override {}
    void set_on_connect(std::function<void()>) override {}
    bool is_connected() const override { return false; }
};

class MockMarketDataFeedForExecutor : public MarketDataFeed {
public:
    MockMarketDataFeedForExecutor() : MarketDataFeed(std::make_shared<DummyWsClient>(), 0) {}
    MOCK_METHOD(void, add_listener, (IMarketDataListener*), (override));
};

class ExecutorStateMachineTest : public ::testing::Test {
protected:
    void SetUp() override {
        Logger::init();
    }
};

TEST_F(ExecutorStateMachineTest, EntryToOpenOnFill) {
    MockOrderGatewayForExecutor gateway;
    MockMarketDataFeedForExecutor feed;
    
    EXPECT_CALL(feed, add_listener(_)).Times(1);
    TickerUniverse universe;
    LiveExecutor executor(1, gateway, feed, universe);

    TradePlan plan;
    plan.ticker = "BTCUSDT";
    plan.side = Side::Buy;
    plan.entry_type = OrderType::Limit;
    plan.entry_price = 50000.0;
    plan.size_coin = 0.1;
    plan.stop_price = 49000.0;
    plan.tp1_price = 52000.0;
    plan.tp1_size_ratio = 0.5;
    
    EXPECT_CALL(gateway, place_order(1, _)).WillOnce(Return(PlaceOrderResult{"Success", "cid1", 10L, 0.0}));
    
    executor.submit(plan);
    
    auto trades = executor.active_trades();
    ASSERT_EQ(trades["BTCUSDT"].size(), 1);
    EXPECT_EQ(trades["BTCUSDT"][0].state, TradeState::PendingEntry);
    
    // Simulate fill
    OrderUpdate upd;
    upd.ticker = "BTCUSDT";
    upd.side = Side::Buy;
    upd.size = 0.1;
    upd.filled_size = 0.1;
    upd.filled_price = 50005.0;
    upd.status = OrderStatus::Closed;
    upd.time = std::chrono::system_clock::now();
    
    // Fill should trigger stops placement (StopLoss and TakeProfit)
    EXPECT_CALL(gateway, place_order(1, Field(&PlaceOrderRequest::type, OrderType::StopLoss))).WillOnce(Return(PlaceOrderResult{"Success", "sl1", 5L, 0.0}));
    EXPECT_CALL(gateway, place_order(1, Field(&PlaceOrderRequest::type, OrderType::TakeProfit))).WillOnce(Return(PlaceOrderResult{"Success", "tp1", 5L, 0.0}));
    
    executor.on_order_update(upd);
    
    trades = executor.active_trades();
    EXPECT_EQ(trades["BTCUSDT"][0].state, TradeState::Open);
    EXPECT_DOUBLE_EQ(trades["BTCUSDT"][0].avg_entry_price, 50005.0);
}
