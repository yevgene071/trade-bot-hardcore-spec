#include <gtest/gtest.h>
#include "transport/MetaScalpCodec.hpp"
#include <nlohmann/json.hpp>

using namespace trade_bot;

TEST(CodecTest, OrderTypeDualParsing) {
    EXPECT_EQ(MetaScalpCodec::parse_order_type("Limit"), OrderType::Limit);
    EXPECT_EQ(MetaScalpCodec::parse_order_type(0), OrderType::Limit);
    
    EXPECT_EQ(MetaScalpCodec::parse_order_type("Stop"), OrderType::Stop);
    EXPECT_EQ(MetaScalpCodec::parse_order_type(1), OrderType::Stop);
    
    EXPECT_EQ(MetaScalpCodec::parse_order_type("StopLoss"), OrderType::StopLoss);
    EXPECT_EQ(MetaScalpCodec::parse_order_type(2), OrderType::StopLoss);
    
    EXPECT_EQ(MetaScalpCodec::parse_order_type("TakeProfit"), OrderType::TakeProfit);
    EXPECT_EQ(MetaScalpCodec::parse_order_type(3), OrderType::TakeProfit);
    
    EXPECT_EQ(MetaScalpCodec::parse_order_type("Market"), OrderType::Market);
    EXPECT_EQ(MetaScalpCodec::parse_order_type(4), OrderType::Market);
    
    EXPECT_THROW(MetaScalpCodec::parse_order_type(7), CodecError);
    EXPECT_THROW(MetaScalpCodec::parse_order_type("Unknown"), CodecError);
}

TEST(CodecTest, OrderUpdateParsing) {
    nlohmann::json j = {
        {"OrderId", 12345},
        {"Ticker", "BTCUSDT"},
        {"Side", "Buy"},
        {"Type", "Limit"},
        {"Price", 60000.5},
        {"FilledPrice", 0.0},
        {"Size", 0.1},
        {"FilledSize", 0.0},
        {"Fee", 0.0},
        {"FeeCurrency", "USDT"},
        {"Status", "New"},
        {"Time", "2024-05-01T12:00:00.123Z"}
    };
    
    auto update = MetaScalpCodec::parse_order_update(j);
    EXPECT_EQ(update.order_id, 12345);
    EXPECT_EQ(update.ticker, "BTCUSDT");
    EXPECT_EQ(update.side, Side::Buy);
    EXPECT_EQ(update.type, OrderType::Limit);
    EXPECT_EQ(update.price, 60000.5);
}

TEST(CodecTest, PositionUpdateAvgPrice) {
    nlohmann::json j = {
        {"ConnectionId", 1},
        {"PositionId", 789},
        {"Ticker", "ETHUSDT"},
        {"Side", "Sell"},
        {"Size", 2.0},
        {"AvgPrice", 2500.0},
        {"AvgPriceFix", 2510.0},
        {"AvgPriceDyn", 2490.0},
        {"Status", "Open"}
    };
    
    auto pos = MetaScalpCodec::parse_position_update(j);
    EXPECT_EQ(pos.avg_price, 2500.0);
    EXPECT_EQ(pos.avg_price_fix, 2510.0);
    EXPECT_EQ(pos.avg_price_dyn, 2490.0);
}

TEST(CodecTest, MissingRequiredField) {
    nlohmann::json j = {
        {"Ticker", "BTCUSDT"}
        // Missing OrderId
    };
    EXPECT_THROW(MetaScalpCodec::parse_order_update(j), CodecError);
}

TEST(CodecTest, FinresUpdateParsing) {
    nlohmann::json j = {
        {"ConnectionId", 1},
        {"Finreses", {
            {
                {"Currency", "USDT"},
                {"Result", 10.5},
                {"Fee", 0.5},
                {"Funds", 1000.0},
                {"Available", 900.0},
                {"Blocked", 100.0}
            }
        }}
    };
    
    auto finres = MetaScalpCodec::parse_finres_update(j);
    ASSERT_EQ(finres.finreses.size(), 1);
    EXPECT_EQ(finres.finreses[0].currency, "USDT");
    EXPECT_EQ(finres.finreses[0].result, 10.5);
}
