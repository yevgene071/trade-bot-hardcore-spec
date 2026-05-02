#include "transport/Clocks.hpp"
#include "transport/ReplayFeed.hpp"
#include "logger/Logger.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

using namespace trade_bot;
using namespace std::chrono_literals;

namespace {

class ReplayTest : public ::testing::Test {
protected:
    void SetUp() override {
        Logger::init();
        path_ = std::filesystem::temp_directory_path() /
                ("replay_test_" + std::to_string(::getpid()) + ".ndjson");
    }
    void TearDown() override {
        std::error_code ec;
        std::filesystem::remove(path_, ec);
    }

    void write_line(std::ofstream& f, int64_t recv_ts_ns, const std::string& msg) {
        f << R"({"recv_ts_ns":)" << recv_ts_ns
          << R"(,"message":)" << msg << "}\n";
    }

    std::filesystem::path path_;
};

class CountingListener : public IMarketDataListener {
public:
    void on_trade(const Ticker&, const Trade&) override {}
    void on_trades(const Ticker& t, const std::vector<Trade>& trades) override {
        last_ticker_ = t;
        trade_batches_.fetch_add(1);
        trade_count_.fetch_add(static_cast<int>(trades.size()));
    }
    void on_orderbook_snapshot(const OrderBookSnapshot&) override {
        snapshots_.fetch_add(1);
    }
    void on_orderbook_update(const OrderBookUpdate&) override {
        updates_.fetch_add(1);
    }
    void on_order_update(const OrderUpdate&) override {}
    void on_position_update(const PositionUpdate&) override {}
    void on_balance_update(const BalanceUpdate&) override {}
    void on_finres_update(const FinresUpdate&) override {}
    void on_error(const std::string&) override {}

    Ticker last_ticker_;
    std::atomic<int> trade_batches_{0};
    std::atomic<int> trade_count_{0};
    std::atomic<int> snapshots_{0};
    std::atomic<int> updates_{0};
};

}  // namespace

TEST_F(ReplayTest, ReplaysTradesSnapshotsAndUpdates) {
    {
        std::ofstream f(path_);
        write_line(f, 1'000'000'000,
            R"({"Type":"trade_update","Data":{"Ticker":"BTCUSDT","Trades":[{"Price":100.0,"Size":0.5,"Side":1,"Time":"2026-05-02T00:00:00.000Z"},{"Price":101.0,"Size":0.25,"Side":2,"Time":"2026-05-02T00:00:00.005Z"}]}})");
        write_line(f, 1'000'500'000,
            R"({"Type":"orderbook_snapshot","Data":{"Ticker":"BTCUSDT","Asks":[{"Price":102.0,"Size":1.0}],"Bids":[{"Price":101.0,"Size":2.0}],"BestAsk":102.0,"BestBid":101.0}})");
        write_line(f, 1'001'000'000,
            R"({"Type":"orderbook_update","Data":{"Ticker":"BTCUSDT","Updates":[{"Price":102.0,"Size":0.0,"Type":1}]}})");
    }

    auto clock = std::make_shared<VirtualClock>();
    ReplayFeed feed{path_.string(), clock, /*speed_multiplier=*/0.0};

    CountingListener listener;
    feed.add_listener(&listener);

    auto stats = feed.run();
    EXPECT_EQ(stats.messages_read, 3u);
    EXPECT_EQ(stats.messages_dispatched, 3u);
    EXPECT_EQ(stats.parse_errors, 0u);

    EXPECT_EQ(listener.trade_batches_, 1);
    EXPECT_EQ(listener.trade_count_, 2);
    EXPECT_EQ(listener.snapshots_, 1);
    EXPECT_EQ(listener.updates_, 1);
    EXPECT_EQ(listener.last_ticker_, "BTCUSDT");
}

TEST_F(ReplayTest, MalformedLinesAreCounted) {
    {
        std::ofstream f(path_);
        f << "this is not json\n";
        write_line(f, 1'000'000'000,
            R"({"Type":"trade_update","Data":{"Ticker":"BTCUSDT","Trades":[{"Price":100.0,"Size":1.0,"Side":1,"Time":"2026-05-02T00:00:00.000Z"}]}})");
        f << R"({"recv_ts_ns":1500000000})" << "\n";  // missing "message"
    }

    auto clock = std::make_shared<VirtualClock>();
    ReplayFeed feed{path_.string(), clock, 0.0};
    CountingListener listener;
    feed.add_listener(&listener);
    auto stats = feed.run();

    EXPECT_GE(stats.parse_errors, 2u);
    EXPECT_EQ(listener.trade_batches_, 1);
}

TEST_F(ReplayTest, RealtimePlaybackUsesClockSleep) {
    {
        std::ofstream f(path_);
        write_line(f, 1'000'000'000,
            R"({"Type":"trade_update","Data":{"Ticker":"BTCUSDT","Trades":[{"Price":100.0,"Size":1.0,"Side":1,"Time":"2026-05-02T00:00:00.000Z"}]}})");
        write_line(f, 1'010'000'000,  // +10 ms
            R"({"Type":"trade_update","Data":{"Ticker":"BTCUSDT","Trades":[{"Price":100.5,"Size":1.0,"Side":1,"Time":"2026-05-02T00:00:00.010Z"}]}})");
    }

    // VirtualClock with sleep_until=no-op + speed=1.0 still produces correct
    // throughput; we only validate that nothing hangs.
    auto clock = std::make_shared<VirtualClock>();
    ReplayFeed feed{path_.string(), clock, /*speed=*/1.0};
    CountingListener listener;
    feed.add_listener(&listener);
    auto stats = feed.run();
    EXPECT_EQ(stats.messages_dispatched, 2u);
}

TEST_F(ReplayTest, EmptyFileYieldsNoEvents) {
    { std::ofstream f(path_); /* truncate to empty */ }
    auto clock = std::make_shared<VirtualClock>();
    ReplayFeed feed{path_.string(), clock, 0.0};
    auto stats = feed.run();
    EXPECT_EQ(stats.messages_read, 0u);
    EXPECT_EQ(stats.messages_dispatched, 0u);
}

TEST_F(ReplayTest, MissingFileThrows) {
    auto clock = std::make_shared<VirtualClock>();
    ReplayFeed feed{"/tmp/__definitely_not_here__.ndjson", clock, 0.0};
    EXPECT_THROW(feed.run(), std::runtime_error);
}
