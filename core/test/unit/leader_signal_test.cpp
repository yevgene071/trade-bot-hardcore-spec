#include "signals/LeaderSignal.hpp"
#include "signals/SignalBus.hpp"
#include "marketdata/LeaderTracker.hpp"
#include "logger/Logger.hpp"
#include <gtest/gtest.h>

using namespace trade_bot;

class LeaderSignalTest : public ::testing::Test {
protected:
    void SetUp() override { Logger::init(); }
    
    FeatureFrame make_frame(Ticker t, double mid, std::chrono::system_clock::time_point ts) {
        FeatureFrame f;
        f.ticker = std::move(t);
        f.mid = mid;
        f.timestamp = ts;
        f.valid = true;
        return f;
    }
};

TEST_F(LeaderSignalTest, DetectsLeaderMove) {
    SignalBus bus;
    LeaderTracker tracker;
    
    LeaderSignal::Config cfg;
    cfg.min_correlation = 0.5;
    cfg.move_min_pct = 0.3;
    cfg.lag_min_pct = 0.1;
    
    LeaderSignal detector("ALTUSDT", "BTCUSDT", bus, tracker, cfg);
    
    int signals = 0;
    bus.subscribe([&signals](const Signal& s){
        if (s.kind == SignalKind::LeaderMove) signals++;
    });
    
    auto now = std::chrono::system_clock::now();
    
    // 1. Warmup both histories and tracker
    for (int i = 0; i < 60; ++i) {
        double btc = 50000.0 + i;
        double alt = 100.0 + i * 0.001;
        tracker.update(btc, alt);
        detector.on_leader_frame(make_frame("BTCUSDT", btc, now));
        detector.on_frame(make_frame("ALTUSDT", alt, now));
        now += std::chrono::milliseconds(100);
    }
    
    // 2. Leader moves up fast, alt lags
    // BTC: 50060 -> 50250 (0.38%)
    // ALT: 100.06 -> 100.07 (0.01%)
    now += std::chrono::seconds(5);
    detector.on_leader_frame(make_frame("BTCUSDT", 50250.0, now));
    detector.on_frame(make_frame("ALTUSDT", 100.07, now));
    
    EXPECT_GT(signals, 0);
}
