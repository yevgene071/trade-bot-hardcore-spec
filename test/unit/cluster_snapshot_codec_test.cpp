#include "transport/MetaScalpCodec.hpp"
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

using namespace trade_bot;

TEST(ClusterSnapshotCodecTest, ParsesValidSnapshot) {
    std::string json_str = R"({
        "Ticker": "BTCUSDT",
        "TimeFrame": "M5",
        "ZoomIndex": 0,
        "Items": [
            {"Price": 100.0, "AskSize": 1.5, "BidSize": 2.5},
            {"Price": 101.0, "AskSize": 0.5, "BidSize": 3.5}
        ]
    })";
    
    auto j = nlohmann::json::parse(json_str);
    auto snap = MetaScalpCodec::parse_cluster_snapshot(j);
    
    EXPECT_EQ(snap.ticker, "BTCUSDT");
    EXPECT_EQ(snap.timeframe, "M5");
    EXPECT_EQ(snap.zoom_index, 0);
    ASSERT_EQ(snap.items.size(), 2);
    
    EXPECT_DOUBLE_EQ(snap.items[0].price, 100.0);
    EXPECT_DOUBLE_EQ(snap.items[0].ask_size, 1.5);
    EXPECT_DOUBLE_EQ(snap.items[0].bid_size, 2.5);
}

TEST(ClusterSnapshotCodecTest, ThrowsOnMissingRequiredFields) {
    std::string json_str = R"({
        "Ticker": "BTCUSDT"
    })";
    auto j = nlohmann::json::parse(json_str);
    EXPECT_THROW(MetaScalpCodec::parse_cluster_snapshot(j), CodecError);
}
