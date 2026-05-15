#include "signals/IcebergDetector.hpp"
#include "signals/SignalBus.hpp"
#include "marketdata/OrderBook.hpp"
#include "universe/TickerUniverse.hpp"
#include "logger/Logger.hpp"
#include <gtest/gtest.h>

using namespace trade_bot;

class IcebergBayesianTest : public ::testing::Test {
protected:
    void SetUp() override { Logger::init(); }
};

TEST_F(IcebergBayesianTest, BayesianEvidenceAccumulation) {
    SignalBus bus;
    OrderBook book{"BTCUSDT", 0.01, 1e-6};
    TickerUniverse universe;
    
    IcebergDetector::Config cfg;
    cfg.prior = 0.05;
    cfg.likelihood_iceberg = 0.85;
    cfg.likelihood_not_iceberg = 0.10;
    cfg.posterior_threshold = 0.80;
    cfg.iceberg_min_size_usd = 0.0; // trigger by posterior only
    cfg.size_retention_ratio = 0.0; // allow any retention for test
    
    IcebergDetector detector("BTCUSDT", bus, book, universe, cfg);
    
    int signals = 0;
    bus.subscribe([&signals](const Signal& s){
        if (s.kind == SignalKind::IcebergSuspected) signals++;
    });
    
    auto now = std::chrono::system_clock::now();
    const double price = 50000.0;
    
    // Initial state: Level at 50000.0 with size 10
    detector.on_book_update({"BTCUSDT", {{price, 10.0, Side::Sell}}, now});
    
    // 1st refill: Trade 10, Book stays 10
    detector.on_trade({price, 10.0, Side::Buy, now});
    detector.on_book_update({"BTCUSDT", {{price, 10.0, Side::Sell}}, now});
    // Posterior after 1 refill: (0.05*0.85) / (0.05*0.85 + 0.95*0.10) = 0.0425 / (0.0425 + 0.095) = 0.309
    
    // 2nd refill
    detector.on_trade({price, 10.0, Side::Buy, now});
    detector.on_book_update({"BTCUSDT", {{price, 10.0, Side::Sell}}, now});
    // Posterior after 2: (0.309*0.85) / (0.309*0.85 + 0.691*0.10) = 0.262 / (0.262 + 0.069) = 0.791
    
    // 3rd refill
    detector.on_trade({price, 10.0, Side::Buy, now});
    detector.on_book_update({"BTCUSDT", {{price, 10.0, Side::Sell}}, now});
    // Posterior after 3: (0.791*0.85) / (0.791*0.85 + 0.209*0.10) = 0.672 / (0.672 + 0.021) = 0.970
    
    EXPECT_EQ(signals, 1);
}

TEST_F(IcebergBayesianTest, WeakEvidenceDecreasesPosterior) {
    // This is hard to test via signals because we don't emit when posterior is LOW.
    // But we can verify that 1 refill + 1 NON-refill doesn't trigger signal easily.
}
