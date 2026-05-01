#include <gtest/gtest.h>
#include "transport/MarketDataFeed.hpp"
#include "transport/BeastWsClient.hpp"
#include "logger/Logger.hpp"
#include <thread>
#include <atomic>

using namespace trade_bot;

class FeedListener : public IMarketDataListener {
public:
    void on_trade(const Ticker& t, const Trade& tr) override { 
        LOG_INFO("Feed received trade for {}: {} @ {}", t, tr.size, tr.price);
        received_trade = true; 
    }
    void on_orderbook_snapshot(const OrderBookSnapshot& s) override { 
        LOG_INFO("Feed received snapshot for {}", s.ticker);
        received_snapshot = true; 
    }
    void on_orderbook_update(const OrderBookUpdate& u) override { received_update = true; }
    void on_order_update(const OrderUpdate& u) override {}
    void on_position_update(const PositionUpdate& u) override {}
    void on_balance_update(const BalanceUpdate& u) override {}
    void on_error(const std::string& msg) override {}

    std::atomic<bool> received_trade{false};
    std::atomic<bool> received_snapshot{false};
    std::atomic<bool> received_update{false};
};

TEST(FeedSmokeTest, LiveData) {
    char* run_integration = std::getenv("WITH_METASCALP");
    if (!run_integration) {
        GTEST_SKIP() << "Skipping integration test: WITH_METASCALP env var not set";
    }

    boost::asio::io_context ioc;
    auto ws = std::make_shared<BeastWsClient>(ioc);
    MarketDataFeed feed(ws, 1);
    FeedListener listener;
    feed.add_listener(&listener);

    ws->connect("ws://127.0.0.1:17845/");
    feed.subscribe_ticker("BTCUSDT");
    feed.start();

    std::thread t([&]() { ioc.run(); });

    // Wait for data
    int retries = 300; // 30 seconds
    while (!(listener.received_trade && listener.received_snapshot) && retries-- > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    EXPECT_TRUE(listener.received_snapshot);
    EXPECT_TRUE(listener.received_trade);

    feed.stop();
    ws->disconnect();
    ioc.stop();
    if (t.joinable()) t.join();
}
