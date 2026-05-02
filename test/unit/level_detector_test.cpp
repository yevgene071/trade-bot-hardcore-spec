#include "signals/LevelDetector.hpp"
#include "signals/SignalBus.hpp"
#include "marketdata/OrderBook.hpp"
#include "marketdata/ClusterSnapshot.hpp"
#include "logger/Logger.hpp"
#include <gtest/gtest.h>
#include <gmock/gmock.h>

using namespace trade_bot;

class FakeHttpClient3 : public IHttpClient {
public:
    HttpResponse get(const std::string&) override { return {200, "{}", {}}; }
    HttpResponse post(const std::string&, const std::string&) override { return {200, "{}", {}}; }
    HttpResponse put(const std::string&, const std::string&) override { return {200, "{}", {}}; }
    HttpResponse del(const std::string&) override { return {200, "{}", {}}; }
    void set_timeout_ms(int) override {}
};

class LevelDetectorTest : public ::testing::Test {
protected:
    void SetUp() override { Logger::init(); }
    
    FeatureFrame make_frame(Ticker t, double mid, std::chrono::system_clock::time_point ts) {
        FeatureFrame f;
        f.ticker = std::move(t);
        f.mid = mid;
        f.timestamp = ts;
        return f;
    }
    
    void push_peak(LevelDetector& d, double price, std::chrono::system_clock::time_point& now) {
        for (int i = 0; i < 5; ++i) {
            d.on_frame(make_frame("BTCUSDT", price - 1.0, now));
            now += std::chrono::milliseconds(100);
        }
        d.on_frame(make_frame("BTCUSDT", price, now));
        now += std::chrono::milliseconds(100);
        for (int i = 0; i < 5; ++i) {
            d.on_frame(make_frame("BTCUSDT", price - 1.0, now));
            now += std::chrono::milliseconds(100);
        }
    }
};

TEST_F(LevelDetectorTest, IdentifiesLevelsFromExtremes) {
    SignalBus bus;
    OrderBook book{"BTCUSDT", 0.01, 1e-6};
    FakeHttpClient3 http;
    ClusterSnapshotClient client(http, "", 0);
    ClusterSnapshotManager cluster_mgr(client);
    
    LevelDetector::Config cfg;
    cfg.touches_min = 2;
    cfg.min_reversal_bps = 5.0;
    cfg.cluster_tolerance_bps = 50.0;
    
    LevelDetector detector("BTCUSDT", bus, book, cluster_mgr, cfg);
    
    auto now = std::chrono::system_clock::now();
    
    // Create 2 peaks
    push_peak(detector, 100.0, now);
    now += std::chrono::seconds(10);
    push_peak(detector, 100.1, now);

    detector.rebuild();
    
    auto levels = detector.levels();
    ASSERT_GE(levels.size(), 1);
    EXPECT_NEAR(levels[0].price, 100.05, 0.1);
    EXPECT_EQ(levels[0].touches, 2);
}
