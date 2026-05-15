#include "features/FeatureExtractor.hpp"
#include "logger/Logger.hpp"
#include "marketdata/OrderBook.hpp"
#include "marketdata/TradeStream.hpp"

#include <gtest/gtest.h>

#include <chrono>

using namespace trade_bot;

namespace {

class FeatureExtractorTest : public ::testing::Test {
protected:
    void SetUp() override { Logger::init(); }

    OrderBook make_book() {
        OrderBook ob{"BTCUSDT", 0.01, 1e-6};
        OrderBookSnapshot s{};
        s.ticker = "BTCUSDT";
        for (int i = 0; i < 10; ++i) {
            const double bid = 100.0 - 0.01 * (i + 1);
            const double ask = 100.0 + 0.01 * (i + 1);
            s.bids.push_back({bid, 1.0 + i * 0.1, Side::Buy});
            s.asks.push_back({ask, 1.0 + i * 0.1, Side::Sell});
        }
        s.ts = std::chrono::system_clock::now();
        ob.apply_snapshot(s);
        return ob;
    }
};

}  // namespace

TEST_F(FeatureExtractorTest, FrameContainsAllOrderbookFields) {
    auto ob = make_book();
    TradeStream ts{"BTCUSDT"};
    FeatureExtractor fe{"BTCUSDT"};
    fe.set_sources(&ob, &ts, /*leader=*/nullptr);

    const auto now = std::chrono::system_clock::now();
    auto frame = fe.extract(now);

    EXPECT_EQ(frame.ticker, "BTCUSDT");
    EXPECT_NEAR(frame.best_bid,    99.99, 1e-9);
    EXPECT_NEAR(frame.best_ask,   100.01, 1e-9);
    EXPECT_NEAR(frame.mid,        100.00, 1e-9);
    EXPECT_NEAR(frame.spread_abs,   0.02, 1e-9);
    EXPECT_NEAR(frame.spread_bps,   2.0,  1e-6);     // 0.02 / 100 * 10000
    EXPECT_NEAR(frame.bid_depth_10, 14.5, 1e-6);     // 1.0..1.9 = 14.5
    EXPECT_NEAR(frame.ask_depth_10, 14.5, 1e-6);
    EXPECT_NEAR(frame.imbalance,     0.0, 1e-9);
}

TEST_F(FeatureExtractorTest, MidHistoryGrowsButStaysBounded) {
    auto ob = make_book();
    TradeStream ts{"BTCUSDT"};
    FeatureExtractor::Config cfg{};
    cfg.reserve_history = 5;
    FeatureExtractor fe{"BTCUSDT", cfg};
    fe.set_sources(&ob, &ts, nullptr);

    auto now = std::chrono::system_clock::now();
    for (int i = 0; i < 20; ++i) {
        fe.extract(now + std::chrono::milliseconds{100 * i});
    }
    EXPECT_LE(fe.history_size(), 5u);
}

TEST_F(FeatureExtractorTest, NoBookYieldsZeroFields) {
    TradeStream ts{"BTCUSDT"};
    FeatureExtractor fe{"BTCUSDT"};
    fe.set_sources(/*ob=*/nullptr, &ts, nullptr);

    auto frame = fe.extract(std::chrono::system_clock::now());
    EXPECT_DOUBLE_EQ(frame.best_bid, 0.0);
    EXPECT_DOUBLE_EQ(frame.spread_bps, 0.0);
    EXPECT_DOUBLE_EQ(frame.imbalance, 0.0);
}

TEST_F(FeatureExtractorTest, LatencyHistogramsRecordSamples) {
    auto ob = make_book();
    TradeStream ts{"BTCUSDT"};
    FeatureExtractor fe{"BTCUSDT"};
    fe.set_sources(&ob, &ts, nullptr);
    auto now = std::chrono::system_clock::now();
    for (int i = 0; i < 100; ++i) {
        fe.extract(now + std::chrono::milliseconds{i});
    }
    EXPECT_EQ(fe.feature_total_hist().total_count(),   100);
    EXPECT_EQ(fe.book_to_feature_hist().total_count(), 100);
}
