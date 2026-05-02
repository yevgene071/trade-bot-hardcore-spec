#include "signals/DensityDetector.hpp"
#include "signals/SignalBus.hpp"
#include "marketdata/OrderBook.hpp"
#include "universe/TickerUniverse.hpp"
#include "logger/Logger.hpp"
#include <gtest/gtest.h>

using namespace trade_bot;

class DensityDetectorTest : public ::testing::Test {
protected:
    void SetUp() override { Logger::init(); }
    
    FeatureFrame make_frame(Ticker t, std::chrono::system_clock::time_point ts) {
        FeatureFrame f;
        f.ticker = std::move(t);
        f.timestamp = ts;
        return f;
    }
    
    DensityDetector::Config make_cfg() {
        DensityDetector::Config cfg;
        cfg.min_size_vs_avg = 1.0;
        cfg.min_size_usd = 100.0; // low for tests
        cfg.min_distance_bps = 1.0;
        return cfg;
    }
};

TEST_F(DensityDetectorTest, DetectsStickyDensity) {
    SignalBus bus;
    OrderBook book{"BTCUSDT", 0.01, 1e-6};
    TickerUniverse universe;
    
    auto cfg = make_cfg();
    cfg.sticky_duration = std::chrono::milliseconds(2000);
    
    DensityDetector detector("BTCUSDT", bus, book, universe, cfg);
    
    int signals = 0;
    bus.subscribe([&signals](const Signal& s){
        if (s.kind == SignalKind::DensityDetected) signals++;
    });
    
    auto now = std::chrono::system_clock::now();
    book.apply_snapshot({"BTCUSDT", {{100.10, 1.0, Side::Sell}}, {{100.00, 1.0, Side::Buy}}, now});
    
    detector.on_book_update({"BTCUSDT", {{100.50, 100.0, Side::Sell}}, now});
    
    detector.on_frame(make_frame("BTCUSDT", now + std::chrono::seconds(1)));
    EXPECT_EQ(signals, 0);
    
    detector.on_frame(make_frame("BTCUSDT", now + std::chrono::seconds(3)));
    EXPECT_EQ(signals, 1);
}

TEST_F(DensityDetectorTest, DetectsFakeDensityRemoval) {
    SignalBus bus;
    OrderBook book{"BTCUSDT", 0.01, 1e-6};
    TickerUniverse universe;
    
    auto cfg = make_cfg();
    cfg.fake_threshold = std::chrono::milliseconds(300);
    
    DensityDetector detector("BTCUSDT", bus, book, universe, cfg);
    
    int fake_signals = 0;
    bus.subscribe([&fake_signals](const Signal& s){
        if (s.kind == SignalKind::DensityRemoved) {
            if (s.payload.value("fake", false)) fake_signals++;
        }
    });
    
    auto now = std::chrono::system_clock::now();
    book.apply_snapshot({"BTCUSDT", {{100.10, 1.0, Side::Sell}}, {{100.00, 1.0, Side::Buy}}, now});
    
    detector.on_book_update({"BTCUSDT", {{100.50, 100.0, Side::Sell}}, now});
    detector.on_book_update({"BTCUSDT", {{100.50, 0.0, Side::Sell}}, now + std::chrono::milliseconds(100)});
    
    EXPECT_EQ(fake_signals, 1);
}

TEST_F(DensityDetectorTest, DetectsDensityEating) {
    SignalBus bus;
    OrderBook book{"BTCUSDT", 0.01, 1e-6};
    TickerUniverse universe;
    
    auto cfg = make_cfg();
    cfg.eating_ratio_threshold = 0.5;
    cfg.eating_min_prints = 2;
    
    DensityDetector detector("BTCUSDT", bus, book, universe, cfg);
    
    int eating_signals = 0;
    bus.subscribe([&eating_signals](const Signal& s){
        if (s.kind == SignalKind::DensityEating) eating_signals++;
    });
    
    auto now = std::chrono::system_clock::now();
    book.apply_snapshot({"BTCUSDT", {{100.10, 1.0, Side::Sell}}, {{100.00, 1.0, Side::Buy}}, now});
    
    detector.on_book_update({"BTCUSDT", {{100.50, 10.0, Side::Sell}}, now});
    
    detector.on_trade({100.50, 3.0, Side::Buy, now + std::chrono::milliseconds(100)});
    EXPECT_EQ(eating_signals, 0);
    
    detector.on_trade({100.50, 3.0, Side::Buy, now + std::chrono::milliseconds(200)});
    EXPECT_EQ(eating_signals, 1);
}
