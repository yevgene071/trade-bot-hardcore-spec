#pragma once

#include "numeric/Welford.hpp"
#include <boost/asio.hpp>
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <mutex>

namespace trade_bot {

class ClockDriftMonitor {
public:
    struct Config {
        std::vector<std::string> sources = {"pool.ntp.org", "time.google.com"};
        int check_interval_sec = 30;
        int warn_drift_ms = 200;
        int max_clock_drift_ms = 500;
    };

    explicit ClockDriftMonitor(const Config& config);
    ~ClockDriftMonitor();

    void start();
    void stop();

    int64_t drift_ms() const;

private:
    void run();
    std::optional<int64_t> fetch_drift();
    std::optional<int64_t> query_ntp(const std::string& host);

    Config config_;
    WelfordAccumulator<double> accumulator_;
    mutable std::mutex mutex_;
    
    std::atomic<bool> running_{false};
    std::thread thread_;
    boost::asio::io_context io_context_;
};

} // namespace trade_bot
