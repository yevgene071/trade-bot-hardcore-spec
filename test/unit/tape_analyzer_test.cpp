#include "signals/TapeAnalyzer.hpp"
#include "signals/SignalBus.hpp"
#include "marketdata/OrderBook.hpp"
#include "marketdata/TradeStream.hpp"
#include "logger/Logger.hpp"
#include <gtest/gtest.h>

using namespace trade_bot;

class TapeAnalyzerTest : public ::testing::Test {
protected:
    void SetUp() override { Logger::init(); }
    
    FeatureFrame make_frame(Ticker t, std::chrono::system_clock::time_point ts) {
        FeatureFrame f;
        f.ticker = std::move(t);
        f.timestamp = ts;
        f.mid = 50000.0;
        return f;
    }
};

TEST_F(TapeAnalyzerTest, DetectsTapeBurst) {
    SignalBus bus;
    OrderBook book{"BTCUSDT", 0.01, 1e-6};
    TradeStream stream{"BTCUSDT", 1.0, 0.3466}; // half-life 2s
    
    TapeAnalyzer analyzer("BTCUSDT", bus, book, stream);
    
    int signals = 0;
    bus.subscribe([&signals](const Signal& s){
        if (s.kind == SignalKind::TapeBurst) signals++;
    });
    
    auto now = std::chrono::system_clock::now();
    
    // 1. Initial state (background intensity)
    for (int i = 0; i < 10; ++i) {
        analyzer.on_frame(make_frame("BTCUSDT", now));
        now += std::chrono::milliseconds(100);
    }
    
    // 2. Burst: 10 trades in 1 second (all Buy)
    for (int i = 0; i < 10; ++i) {
        Trade t{50000.0, 1.0, Side::Buy, now};
        stream.on_trade(t);
        stream.update(now);
        analyzer.on_frame(make_frame("BTCUSDT", now));
        now += std::chrono::milliseconds(100);
    }
    
    EXPECT_GT(signals, 0);
}

TEST_F(TapeAnalyzerTest, DetectsTapeFade) {
    SignalBus bus;
    OrderBook book{"BTCUSDT", 0.01, 1e-6};
    TradeStream stream{"BTCUSDT", 1.0, 0.3466};
    
    TapeAnalyzer::Config cfg;
    cfg.fade_cusum_h = 2.0; // sensitive for test
    TapeAnalyzer analyzer("BTCUSDT", bus, book, stream, cfg);
    
    int signals = 0;
    bus.subscribe([&signals](const Signal& s){
        if (s.kind == SignalKind::TapeFade) signals++;
    });
    
    auto now = std::chrono::system_clock::now();
    
    // 1. Create a peak
    for (int i = 0; i < 20; ++i) {
        stream.on_trade({50000.0, 1.0, Side::Buy, now});
        stream.update(now);
        analyzer.on_frame(make_frame("BTCUSDT", now));
        now += std::chrono::milliseconds(50);
    }
    
    // 2. Sudden stop
    for (int i = 0; i < 100; ++i) { // wait up to 10s
        stream.update(now);
        analyzer.on_frame(make_frame("BTCUSDT", now));
        now += std::chrono::milliseconds(100);
        if (signals > 0) break;
    }
    
    EXPECT_GT(signals, 0);
}

TEST_F(TapeAnalyzerTest, DetectsTapeFlush) {
    SignalBus bus;
    OrderBook book{"BTCUSDT", 0.01, 1e-6};
    TradeStream stream{"BTCUSDT", 1.0, 0.3466};
    
    TapeAnalyzer::Config cfg;
    cfg.flush_min_size_usd = 100000.0;
    cfg.flush_min_move_bps = 1.0; // sensitive
    
    TapeAnalyzer analyzer("BTCUSDT", bus, book, stream, cfg);
    
    int signals = 0;
    bus.subscribe([&signals](const Signal& s){
        if (s.kind == SignalKind::TapeFlush) signals++;
    });
    
    auto now = std::chrono::system_clock::now();
    book.apply_snapshot({"BTCUSDT", {{50010.0, 1.0, Side::Sell}}, {{50000.0, 1.0, Side::Buy}}, now});
    
    // Normal trades to populate T-Digest
    for (int i = 0; i < 100; ++i) {
        stream.on_trade({50005.0, 1.0, Side::Buy, now});
    }
    stream.update(now);
    
    // Large trade (outlier) with price move
    // mid was 50005.0. Trade at 50100.0 is ~19 bps move.
    Trade t{50100.0, 10.0, Side::Buy, now}; // 501000 USD
    analyzer.on_trade(t);
    
    EXPECT_EQ(signals, 1);
}
