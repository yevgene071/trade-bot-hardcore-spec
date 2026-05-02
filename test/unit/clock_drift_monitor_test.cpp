#include "control/ClockDriftMonitor.hpp"
#include "control/INtpClient.hpp"
#include "logger/Logger.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <string>

using namespace trade_bot;
using namespace std::chrono_literals;

namespace {

class FakeNtpClient : public INtpClient {
public:
    /// `offsets[host] = how_far_ahead_server_appears_vs_local`
    std::map<std::string, std::chrono::milliseconds> offsets;
    std::map<std::string, bool>                       reject;
    std::atomic<int>                                  call_count{0};
    std::vector<std::string>                          calls;
    std::mutex                                        mtx;

    std::optional<std::chrono::system_clock::time_point>
    query(const std::string& host, int /*timeout_ms*/) override {
        ++call_count;
        {
            std::lock_guard<std::mutex> lk(mtx);
            calls.push_back(host);
        }
        if (reject.count(host) && reject[host]) {
            return std::nullopt;
        }
        if (!offsets.count(host)) {
            return std::nullopt;
        }
        // server_time = local_time - offset → drift_sample = local - server = offset.
        return std::chrono::system_clock::now() - offsets[host];
    }
};

class ClockDriftMonitorTest : public ::testing::Test {
protected:
    void SetUp() override { Logger::init(); }
};

}  // namespace

TEST_F(ClockDriftMonitorTest, NoNtpClientReturnsNullopt) {
    ClockDriftMonitor mon{nullptr};
    EXPECT_EQ(mon.tick_once(), std::nullopt);
}

TEST_F(ClockDriftMonitorTest, ReportsDriftFromSingleSource) {
    auto ntp = std::make_shared<FakeNtpClient>();
    ntp->offsets["a"] = 250ms;

    ClockDriftMonitor::Config cfg{};
    cfg.sources           = {"a"};
    cfg.moving_avg_window = 1;
    ClockDriftMonitor mon{ntp, cfg};

    auto drift = mon.tick_once();
    ASSERT_TRUE(drift.has_value());
    EXPECT_GE(*drift, 200);
    EXPECT_LE(*drift, 320);  // headroom for syscall jitter
}

TEST_F(ClockDriftMonitorTest, FailoverToNextSource) {
    auto ntp = std::make_shared<FakeNtpClient>();
    ntp->reject["primary"]   = true;       // primary rejects
    ntp->offsets["fallback"] = 50ms;        // fallback responds

    ClockDriftMonitor::Config cfg{};
    cfg.sources           = {"primary", "fallback"};
    cfg.moving_avg_window = 1;
    ClockDriftMonitor mon{ntp, cfg};

    auto drift = mon.tick_once();
    ASSERT_TRUE(drift.has_value());
    EXPECT_NEAR(*drift, 50, 70);

    std::lock_guard<std::mutex> lk(ntp->mtx);
    ASSERT_GE(ntp->calls.size(), 2u);
    EXPECT_EQ(ntp->calls[0], "primary");
    EXPECT_EQ(ntp->calls[1], "fallback");
}

TEST_F(ClockDriftMonitorTest, AllSourcesUnreachable) {
    auto ntp = std::make_shared<FakeNtpClient>();
    ntp->reject["a"] = true;
    ntp->reject["b"] = true;

    ClockDriftMonitor::Config cfg{};
    cfg.sources = {"a", "b"};
    ClockDriftMonitor mon{ntp, cfg};

    EXPECT_EQ(mon.tick_once(), std::nullopt);
}

TEST_F(ClockDriftMonitorTest, WarnDoesNotTriggerKill) {
    auto ntp = std::make_shared<FakeNtpClient>();
    ntp->offsets["a"] = 300ms;  // > warn_drift_ms (200), < max_clock_drift_ms (500)

    ClockDriftMonitor::Config cfg{};
    cfg.sources           = {"a"};
    cfg.moving_avg_window = 1;
    ClockDriftMonitor mon{ntp, cfg};

    bool kill_called = false;
    mon.set_kill_switch([&](const std::string&) { kill_called = true; });

    mon.tick_once();
    EXPECT_FALSE(kill_called);
}

TEST_F(ClockDriftMonitorTest, ExceedingMaxDriftTriggersKillSwitchOnce) {
    auto ntp = std::make_shared<FakeNtpClient>();
    ntp->offsets["a"] = 800ms;  // > max_clock_drift_ms (500)

    ClockDriftMonitor::Config cfg{};
    cfg.sources           = {"a"};
    cfg.moving_avg_window = 1;
    ClockDriftMonitor mon{ntp, cfg};

    int kill_calls = 0;
    std::string reason_seen;
    mon.set_kill_switch([&](const std::string& reason) {
        ++kill_calls;
        reason_seen = reason;
    });

    mon.tick_once();
    mon.tick_once();   // second poll should NOT re-trigger
    EXPECT_EQ(kill_calls, 1);
    EXPECT_NE(reason_seen.find("ClockDrift"), std::string::npos);
}

TEST_F(ClockDriftMonitorTest, MovingAverageSmoothsSpikes) {
    auto ntp = std::make_shared<FakeNtpClient>();

    ClockDriftMonitor::Config cfg{};
    cfg.sources           = {"a"};
    cfg.moving_avg_window = 4;
    cfg.warn_drift_ms     = 200;
    cfg.max_clock_drift_ms = 500;
    ClockDriftMonitor mon{ntp, cfg};

    int kill_calls = 0;
    mon.set_kill_switch([&](const std::string&) { ++kill_calls; });

    // Three small samples + one big spike → smoothed below max.
    ntp->offsets["a"] = 50ms;   mon.tick_once();
    ntp->offsets["a"] = 50ms;   mon.tick_once();
    ntp->offsets["a"] = 50ms;   mon.tick_once();
    ntp->offsets["a"] = 1500ms; mon.tick_once();   // spike

    EXPECT_EQ(kill_calls, 0);  // smoothed (~412 ms) < max
    EXPECT_LT(std::abs(mon.drift_ms()), cfg.max_clock_drift_ms);
}
