#include "ClockDriftMonitor.hpp"

#include "logger/Logger.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <numeric>

namespace trade_bot {

ClockDriftMonitor::ClockDriftMonitor(std::shared_ptr<INtpClient> ntp)
    : ClockDriftMonitor(std::move(ntp), Config{}) {}

ClockDriftMonitor::ClockDriftMonitor(std::shared_ptr<INtpClient> ntp, Config cfg)
    : cfg_(std::move(cfg))
    , ntp_(std::move(ntp))
    , window_(cfg_.moving_avg_window, 0) {}

ClockDriftMonitor::~ClockDriftMonitor() { stop(); }

void ClockDriftMonitor::set_kill_switch(KillSwitchCallback cb) {
    std::lock_guard<std::mutex> lk(mtx_);
    kill_cb_ = std::move(cb);
}

void ClockDriftMonitor::start() {
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true)) {
        return;
    }
    thread_ = std::thread([this] { run_loop_(); });
}

void ClockDriftMonitor::stop() {
    bool expected = true;
    if (!running_.compare_exchange_strong(expected, false)) {
        return;
    }
    if (thread_.joinable()) {
        thread_.join();
    }
}

void ClockDriftMonitor::run_loop_() {
    while (running_.load()) {
        tick_once();
        // sleep in small slices so stop() is responsive
        const auto end = std::chrono::steady_clock::now() + cfg_.check_interval;
        while (running_.load() && std::chrono::steady_clock::now() < end) {
            std::this_thread::sleep_for(std::chrono::milliseconds{100});
        }
    }
}

std::optional<int64_t> ClockDriftMonitor::tick_once() {
    if (!ntp_) {
        return std::nullopt;
    }

    std::optional<std::chrono::system_clock::time_point> server_time;
    for (const auto& src : cfg_.sources) {
        server_time = ntp_->query(src, static_cast<int>(cfg_.query_timeout.count()));
        if (server_time) break;
        LOG_WARN("ClockDriftMonitor: {} did not respond, trying next source", src);
    }
    if (!server_time) {
        LOG_WARN("ClockDriftMonitor: all NTP sources unreachable this cycle");
        return std::nullopt;
    }

    const auto local = std::chrono::system_clock::now();
    const int64_t sample_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(local - *server_time).count();

    update_average_(sample_ms);

    int64_t smoothed{};
    {
        // T4-CONCURRENCY: Use unique_lock to allow safe manual unlock/relock (#138)
        std::unique_lock<std::mutex> lk(mtx_);
        smoothed = drift_ms_;
        evaluate_thresholds_locked_(lk);
    }
    return smoothed;
}

void ClockDriftMonitor::update_average_(int64_t sample) {
    std::lock_guard<std::mutex> lk(mtx_);
    if (window_.empty()) {
        drift_ms_ = sample;
        return;
    }
    window_[window_idx_] = sample;
    window_idx_ = (window_idx_ + 1) % window_.size();
    // Track fill count so cold-start zeros don't dilute early measurements.
    if (window_fill_ < window_.size()) ++window_fill_;
    const int64_t sum = std::accumulate(window_.begin(),
                                        window_.begin() + static_cast<std::ptrdiff_t>(window_fill_),
                                        int64_t{0});
    drift_ms_ = sum / static_cast<int64_t>(window_fill_);
}

void ClockDriftMonitor::evaluate_thresholds_locked_(std::unique_lock<std::mutex>& lk) {
    const int64_t abs_drift = std::abs(drift_ms_);
    if (abs_drift >= cfg_.max_clock_drift_ms) {
        if (!kill_triggered_ && kill_cb_) {
            kill_triggered_ = true;
            // run callback outside the lock to avoid reentrancy hazards
            auto cb = kill_cb_;
            lk.unlock();
            cb("ClockDrift exceeds " + std::to_string(cfg_.max_clock_drift_ms) +
               " ms (smoothed=" + std::to_string(drift_ms_) + ")");
            lk.lock();
        }
        LOG_ERROR("ClockDriftMonitor: drift={} ms exceeds max {} ms — kill triggered",
                  drift_ms_, cfg_.max_clock_drift_ms);
    } else if (abs_drift >= cfg_.warn_drift_ms) {
        LOG_WARN("ClockDriftMonitor: drift={} ms exceeds warn {} ms",
                 drift_ms_, cfg_.warn_drift_ms);
    } else {
        // Drift recovered below warn threshold — allow re-triggering on next transgression.
        kill_triggered_ = false;
    }
}

int64_t ClockDriftMonitor::drift_ms() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return drift_ms_;
}

}  // namespace trade_bot
