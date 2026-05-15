#include "marketdata/ClusterSnapshot.hpp"
#include "logger/Logger.hpp"
#include <gtest/gtest.h>
#include <gmock/gmock.h>

using namespace trade_bot;

class MockClusterSnapshotClient : public ClusterSnapshotClient {
public:
    MockClusterSnapshotClient(IHttpClient& http) : ClusterSnapshotClient(http, "", 0) {}
    MOCK_METHOD(std::optional<ClusterSnapshot>, fetch, (const Ticker&, const std::string&, int), (override));
};

// Fake HttpClient to satisfy constructor
class FakeHttpClient : public IHttpClient {
public:
    HttpResponse get(const std::string&) override { 
        return HttpResponse{.status = 200, .body = "{}", .headers = {}}; 
    }
    HttpResponse post(const std::string&, const std::string&) override { 
        return HttpResponse{.status = 200, .body = "{}", .headers = {}}; 
    }
    HttpResponse put(const std::string&, const std::string&) override { 
        return HttpResponse{.status = 200, .body = "{}", .headers = {}}; 
    }
    HttpResponse del(const std::string&) override { 
        return HttpResponse{.status = 200, .body = "{}", .headers = {}}; 
    }
    void set_timeout_ms(int) override {}
};

class ClusterSnapshotManagerTest : public ::testing::Test {
protected:
    void SetUp() override { Logger::init(); }
};

TEST_F(ClusterSnapshotManagerTest, PollerUpdatesCache) {
    FakeHttpClient http;
    MockClusterSnapshotClient client(http);
    
    ClusterSnapshotManager::Config cfg;
    cfg.poll_interval_sec = std::chrono::seconds(1);
    cfg.poll_timeframes = {"M5"};
    
    ClusterSnapshotManager manager(client, cfg);
    manager.refresh({"BTCUSDT"});
    
    ClusterSnapshot mock_snap {
        .ticker = "BTCUSDT",
        .timeframe = "M5",
        .zoom_index = 0,
        .items = {{100.0, 1.0, 1.0}},
        .ts = std::chrono::system_clock::now()
    };
    
    EXPECT_CALL(client, fetch("BTCUSDT", "M5", 0))
        .WillRepeatedly(testing::Return(mock_snap));
        
    manager.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    
    auto snap = manager.get("BTCUSDT", "M5");
    ASSERT_TRUE(snap.has_value());
    EXPECT_EQ(snap->ticker, "BTCUSDT");
    EXPECT_EQ(snap->items.size(), 1);
    
    manager.stop();
}
