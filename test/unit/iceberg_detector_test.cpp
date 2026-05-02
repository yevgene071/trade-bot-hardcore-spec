#include "signals/IcebergDetector.hpp"
#include "signals/SignalBus.hpp"
#include "marketdata/OrderBook.hpp"
#include "universe/TickerUniverse.hpp"
#include "logger/Logger.hpp"
#include <gtest/gtest.h>

using namespace trade_bot;

class IcebergDetectorTest : public ::testing::Test {
protected:
    void SetUp() override { Logger::init(); }
};

TEST_F(IcebergDetectorTest, DetectsIcebergOnRefill) {
    SignalBus bus;
    OrderBook book{"BTCUSDT", 0.01, 1e-6};
    TickerUniverse universe;
    
    IcebergDetector::Config cfg;
    cfg.iceberg_min_size_usd = 1000.0;
    cfg.evidence_count_min = 2; // legacy mode test or just posterior
    cfg.posterior_threshold = 0.5; // low for test
    
    IcebergDetector detector("BTCUSDT", bus, book, universe, cfg);
    
    int signals = 0;
    bus.subscribe([&signals](const Signal& s){
        if (s.kind == SignalKind::IcebergSuspected) signals++;
    });
    
    auto now = std::chrono::system_clock::now();
    const double price = 50000.0;
    
    // 1. Initial level
    detector.on_book_update({"BTCUSDT", {{price, 10.0, Side::Sell}}, now});
    
    // 2. Refill 1: Trade 10, Book stays 10
    detector.on_trade({price, 10.0, Side::Buy, now});
    detector.on_book_update({"BTCUSDT", {{price, 10.0, Side::Sell}}, now});
    
    // 3. Refill 2: Trade 10, Book stays 10
    detector.on_trade({price, 10.0, Side::Buy, now});
    detector.on_book_update({"BTCUSDT", {{price, 10.0, Side::Sell}}, now});
    
    EXPECT_EQ(signals, 1);
}

TEST_F(IcebergDetectorTest, NoIcebergOnNormalConsumption) {
    SignalBus bus;
    OrderBook book{"BTCUSDT", 0.01, 1e-6};
    TickerUniverse universe;
    
    IcebergDetector detector("BTCUSDT", bus, book, universe);
    
    int signals = 0;
    bus.subscribe([&signals](const Signal& s){
        if (s.kind == SignalKind::IcebergSuspected) signals++;
    });
    
    auto now = std::chrono::system_clock::now();
    const double price = 50000.0;
    
    detector.on_book_update({"BTCUSDT", {{price, 10.0, Side::Sell}}, now});
    
    // Trade 5, Book becomes 5 -> exact visible decrease
    detector.on_trade({price, 5.0, Side::Buy, now});
    detector.on_book_update({"BTCUSDT", {{price, 5.0, Side::Sell}}, now});
    
    EXPECT_EQ(signals, 0);
}
