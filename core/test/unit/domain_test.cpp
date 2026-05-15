#include <gtest/gtest.h>
#include "domain/Types.hpp"

using namespace trade_bot;

TEST(DomainTypesTest, EqualityOperators) {
    PriceLevel p1{65000.0, 1.0, Side::Buy};
    PriceLevel p2{65000.0, 1.0, Side::Buy};
    PriceLevel p3{65000.0, 1.1, Side::Buy};

    EXPECT_EQ(p1, p2);
    EXPECT_NE(p1, p3);

    Ticker t = "BTCUSDT";
    EXPECT_EQ(t, "BTCUSDT");

    BalanceEntry b1{"USDT", 1000.0, 800.0, 200.0};
    BalanceEntry b2{"USDT", 1000.0, 800.0, 200.0};
    EXPECT_EQ(b1, b2);
}

TEST(DomainTypesTest, OrderUpdateEquality) {
    auto now = std::chrono::system_clock::now();
    OrderUpdate o1{123, "BTCUSDT", Side::Buy, OrderType::Limit, 60000.0, 0.0, 0.1, 0.0, 0.0, "USDT", OrderStatus::New, now};
    OrderUpdate o2 = o1;
    EXPECT_EQ(o1, o2);

    o2.filled_size = 0.05;
    EXPECT_NE(o1, o2);
}

TEST(DomainTypesTest, PositionUpdate) {
    PositionUpdate pos{1, 456, "ETHUSDT", Side::Sell, 2.0, 2500.0, 2500.0, 2500.0, 0.01, PositionStatus::Open};
    EXPECT_EQ(pos.ticker, "ETHUSDT");
    EXPECT_EQ(pos.avg_price_fix, 2500.0);
}
