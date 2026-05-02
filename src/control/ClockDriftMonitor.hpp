#pragma once

#include "INtpClient.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace trade_bot {

/**
 * T0-CLOCK: monitors drift between local system_clock and an NTP reference.
 *
 * Behaviour:
 *   - Polls each `sources` host in turn; first successful reply wins (failover).
 *   - Drift is the rolling moving average of the last N samples
 *     (Knuth-style online mean) — single-sample spikes don't immediately fire.
 *   - |drift| >= warn_drift_ms     → WARN log
 *   - |drift| >= max_clock_drift_ms → kill_switch_callback (operator must
 *     decide what to do — typically `KillSwitch::trigger("ClockDrift")` once
 *     T0-KILLSWITCH lands; until then the callback is a no-op).
 *
 * The poller runs on a background thread; tests can drive a single iteration
 * via `tick_once()` to keep them deterministic.
 */
class ClockDriftMonitor {
public:
    using KillSwitchCallback = std::function<void(const std::string& reason)>;

    struct Config {
        std::vector<std::string>  sources{"pool.ntp.org", "time.google.com"};
        std::chrono::seconds      check_interval{30};
        std::chrono::milliseconds query_timeout{1500};
        int64_t                   warn_drift_ms{200};
        int64_t                   max_clock_drift_ms{500};
        // Window for the Knuth-style rolling mean. Per spec AC
        // ("+700 ms → 1 poll WARN, 2 polls KILL") a window of 2 dilutes a
        // single sample below the kill threshold while still crossing warn,
        // and a second matching sample averages back across max.
        size_t                    moving_avg_window{2};
    };

    explicit ClockDriftMonitor(std::shared_ptr<INtpClient> ntp);
    ClockDriftMonitor(std::shared_ptr<INtpClient> ntp, Config cfg);
    ~ClockDriftMonitor();

    ClockDriftMonitor(const ClockDriftMonitor&)            = delete;
    ClockDriftMonitor& operator=(const ClockDriftMonitor&) = delete;

    void set_kill_switch(KillSwitchCallback cb);

    /// Start the background poll loop. Subsequent calls are no-ops.
    void start();
    void stop();

    /// Run a single poll iteration synchronously. Returns the resulting
    /// (smoothed) drift in milliseconds, or std::nullopt if all sources failed.
    std::optional<int64_t> tick_once();

    int64_t drift_ms() const;

private:
    void run_loop_();
    void update_average_(int64_t new_sample_ms);
    void evaluate_thresholds_locked_();

    Config                       cfg_;
    std::shared_ptr<INtpClient>  ntp_;
    KillSwitchCallback           kill_cb_;

    mutable std::mutex           mtx_;
    std::vector<int64_t>         window_;       // ring buffer of last N samples
    size_t                       window_idx_{0};
    int64_t                      drift_ms_{0};
    bool                         kill_triggered_{false};

    std::atomic<bool>            running_{false};
    std::thread                  thread_;
};

}  // namespace trade_bot
