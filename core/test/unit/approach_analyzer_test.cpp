#include "signals/ApproachAnalyzer.hpp"
#include "signals/SignalBus.hpp"
#include "marketdata/OrderBook.hpp"
#include "signals/LevelDetector.hpp"
#include "marketdata/ClusterSnapshot.hpp"
#include "logger/Logger.hpp"
#include <gtest/gtest.h>

using namespace trade_bot;

class ApproachAnalyzerTest : public ::testing::Test {
protected:
    void SetUp() override { Logger::init(); }
    
    FeatureFrame make_frame(Ticker t, double mid, std::chrono::system_clock::time_point ts) {
        FeatureFrame f;
        f.ticker = std::move(t);
        f.mid = mid;
        f.timestamp = ts;
        return f;
    }
};

class FakeHttpClient4 : public IHttpClient {
public:
    HttpResponse get(const std::string&) override { return {200, "{}", {}}; }
    HttpResponse post(const std::string&, const std::string&) override { return {200, "{}", {}}; }
    HttpResponse put(const std::string&, const std::string&) override { return {200, "{}", {}}; }
    HttpResponse del(const std::string&) override { return {200, "{}", {}}; }
    void set_timeout_ms(int) override {}
};

TEST_F(ApproachAnalyzerTest, ClassifiesImpulseApproach) {
    SignalBus bus;
    OrderBook book{"BTCUSDT", 0.01, 1e-6};
    FakeHttpClient4 http;
    ClusterSnapshotClient client(http, "", 0);
    ClusterSnapshotManager cluster_mgr(client);
    LevelDetector level_detector("BTCUSDT", bus, book, cluster_mgr);
    
    ApproachAnalyzer analyzer("BTCUSDT", bus, book, level_detector);
    
    auto now = std::chrono::system_clock::now();
    for (int i = 0; i < 30; ++i) {
        // Target: 90 bps move in 30s = 3 bps/sec speed.
        // 90 bps at 100 is 0.9.
        analyzer.on_frame(make_frame("BTCUSDT", 100.0 + i * (0.9/30.0), now));
        now += std::chrono::seconds(1);
    }
    
    auto res = analyzer.analyze(101.0, now);
    EXPECT_EQ(res.type, ApproachAnalyzer::ApproachType::Impulse);
}
