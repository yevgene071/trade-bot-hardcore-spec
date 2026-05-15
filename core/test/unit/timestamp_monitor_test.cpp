#include "perf/TimestampMonitor.hpp"
#include "logger/Logger.hpp"

#include <gtest/gtest.h>

#include <thread>

using namespace trade_bot;
using namespace std::chrono_literals;

namespace {

class TimestampMonitorTest : public ::testing::Test {
protected:
    void SetUp() override { Logger::init(); }
};

Trade make_trade() {
    return Trade{
        /*price=*/100.0,
        /*size=*/1.0,
        /*side=*/Side::Buy,
        /*timestamp=*/std::chrono::system_clock::now(),
    };
}

OrderBookUpdate make_book_update(const Ticker& ticker) {
    return OrderBookUpdate{
        /*ticker=*/ticker,
        /*changes=*/{},
        /*ts=*/std::chrono::system_clock::now(),
    };
}

} // namespace

TEST_F(TimestampMonitorTest, NoSamplesUntilCounterpartArrives) {
    TimestampMonitor mon{};
    mon.on_trade("BTCUSDT", make_trade());
    mon.on_trade("BTCUSDT", make_trade());
    EXPECT_EQ(mon.sample_count(), 0);
}

TEST_F(TimestampMonitorTest, RecordsCrossStreamPair) {
    TimestampMonitor mon{};
    mon.on_trade("BTCUSDT", make_trade());
    std::this_thread::sleep_for(2ms);
    mon.on_orderbook_update(make_book_update("BTCUSDT"));
    EXPECT_EQ(mon.sample_count(), 1);
    EXPECT_GE(mon.latency_p50_us(), 1'000);
}

TEST_F(TimestampMonitorTest, TickersDoNotCrossPair) {
    TimestampMonitor mon{};
    mon.on_trade("BTCUSDT", make_trade());
    mon.on_orderbook_update(make_book_update("ETHUSDT"));
    EXPECT_EQ(mon.sample_count(), 0);
}

TEST_F(TimestampMonitorTest, EventsBeyondPairWindowAreDropped) {
    TimestampMonitor::Config cfg{};
    cfg.pair_window_us = 1'000;  // 1 ms
    TimestampMonitor mon{cfg};

    mon.on_trade("BTCUSDT", make_trade());
    std::this_thread::sleep_for(5ms);  // exceeds window
    mon.on_orderbook_update(make_book_update("BTCUSDT"));
    EXPECT_EQ(mon.sample_count(), 0);
}

TEST_F(TimestampMonitorTest, JitterReflectsVariance) {
    TimestampMonitor mon{};
    // Alternate trade/book with varying gaps to produce non-zero stddev.
    for (int i = 0; i < 50; ++i) {
        mon.on_trade("BTCUSDT", make_trade());
        std::this_thread::sleep_for(std::chrono::microseconds(200 + (i % 5) * 100));
        mon.on_orderbook_update(make_book_update("BTCUSDT"));
        std::this_thread::sleep_for(std::chrono::microseconds(150));
    }
    EXPECT_GT(mon.sample_count(), 50);
    EXPECT_GT(mon.jitter_us(), 0.0);
}

TEST_F(TimestampMonitorTest, ResetClearsState) {
    TimestampMonitor mon{};
    mon.on_trade("BTCUSDT", make_trade());
    std::this_thread::sleep_for(1ms);
    mon.on_orderbook_update(make_book_update("BTCUSDT"));
    ASSERT_GT(mon.sample_count(), 0);
    mon.reset();
    EXPECT_EQ(mon.sample_count(), 0);
}
