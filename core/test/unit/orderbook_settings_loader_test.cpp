#include "universe/OrderbookSettingsLoader.hpp"
#include "universe/TickerUniverse.hpp"
#include "transport/MetaScalpCodec.hpp"
#include "logger/Logger.hpp"
#include <gtest/gtest.h>
#include <gmock/gmock.h>

using namespace trade_bot;

class MockHttpClient : public IHttpClient {
public:
    MOCK_METHOD(HttpResponse, get, (const std::string&), (override));
    MOCK_METHOD(HttpResponse, post, (const std::string&, const std::string&), (override));
    MOCK_METHOD(HttpResponse, put, (const std::string&, const std::string&), (override));
    MOCK_METHOD(HttpResponse, del, (const std::string&), (override));
    MOCK_METHOD(void, set_timeout_ms, (int), (override));
};

TEST(OrderbookSettingsLoaderTest, ParsesValidResponse) {
    MockHttpClient http;
    OrderbookSettingsLoader loader(http, "http://localhost", 1);
    
    std::string body = R"({
        "Ticker": "BTCUSDT",
        "LargeAmountUsd": 250000.0,
        "LargeAmountUsd2": 500000.0
    })";
    
    EXPECT_CALL(http, get(testing::_))
        .WillOnce(testing::Return(HttpResponse{200, body, {}}));
        
    auto res = loader.fetch("BTCUSDT");
    ASSERT_TRUE(res.has_value());
    EXPECT_EQ(res->ticker, "BTCUSDT");
    EXPECT_DOUBLE_EQ(res->large_amount_usd, 250000.0);
}

TEST(DensityCalibrationTest, UsesMaxOfConfigAndSettings) {
    TickerUniverse universe;
    universe.update_orderbook_settings({"BTCUSDT", 250000.0, 500000.0});
    
    // Case 1: Config is lower -> use settings
    EXPECT_DOUBLE_EQ(universe.density_min_size_usd("BTCUSDT", 50000.0), 250000.0);
    
    // Case 2: Config is higher -> use config
    EXPECT_DOUBLE_EQ(universe.density_min_size_usd("BTCUSDT", 300000.0), 300000.0);
    
    // Case 3: Unknown ticker -> use config
    EXPECT_DOUBLE_EQ(universe.density_min_size_usd("ETHUSDT", 50000.0), 50000.0);
}
