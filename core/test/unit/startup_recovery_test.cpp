#include "executor/StartupRecovery.hpp"
#include "transport/IHttpClient.hpp"
#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include <filesystem>

using namespace trade_bot;
using namespace testing;

class MockOrderGateway : public IOrderGateway {
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

TEST(StartupRecoveryTest, RecoversCleanState) {
    const std::string path = "test_recovery_clean.json";
    if (std::filesystem::exists(path)) std::filesystem::remove(path);
    
    AccountStatePersister persister(path);
    MockOrderGateway gateway;
    StartupRecovery recovery(1, gateway, persister, {});

    AccountStatePersister::PersistedData data;
    ActiveTrade t;
    t.plan.ticker = "BTCUSDT";
    data.active_trades.push_back(t);
    persister.save(data);
    
    std::vector<PositionUpdate> positions;
    positions.push_back({ 1, 100, "BTCUSDT", Side::Buy, 1.0, 50000.0, 50000.0, 50000.0, 0.01, PositionStatus::Open });
    EXPECT_CALL(gateway, get_positions(1)).WillOnce(Return(positions));
    
    EXPECT_CALL(gateway, get_open_orders(1, "BTCUSDT")).WillOnce(Return(std::vector<RestOrder>{}));
    
    auto res = recovery.run();
    EXPECT_TRUE(res.auto_ack);
    ASSERT_EQ(res.recovered_trades.size(), 1);
    EXPECT_EQ(res.recovered_trades[0].plan.ticker, "BTCUSDT");
    EXPECT_EQ(res.recovered_trades[0].state, TradeState::Open);
    
    std::filesystem::remove(path);
}

TEST(StartupRecoveryTest, DetectsDrift) {
    const std::string path = "test_recovery_drift.json";
    if (std::filesystem::exists(path)) std::filesystem::remove(path);
    
    AccountStatePersister persister(path);
    MockOrderGateway gateway;
    StartupRecovery recovery(1, gateway, persister, {});

    // No persisted trades
    
    std::vector<PositionUpdate> positions;
    positions.push_back({ 1, 100, "BTCUSDT", Side::Buy, 1.0, 50000.0, 50000.0, 50000.0, 0.01, PositionStatus::Open });
    EXPECT_CALL(gateway, get_positions(1)).WillOnce(Return(positions));
    EXPECT_CALL(gateway, get_open_orders(1, "BTCUSDT")).WillOnce(Return(std::vector<RestOrder>{}));

    auto res = recovery.run();
    // No persisted state found -> clean startup? 
    // Wait, if no persisted state but position found -> orphan.
    // res.auto_ack will be false if drift detected.
    
    EXPECT_FALSE(res.auto_ack);
    EXPECT_EQ(res.recovered_trades.size(), 1); 
    
    std::filesystem::remove(path);
}
