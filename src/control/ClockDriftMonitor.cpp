#include "ClockDriftMonitor.hpp"
#include "KillSwitch.hpp"
#include "logger/Logger.hpp"

#include <chrono>
#include <iostream>
#include <cstring>
#include <boost/asio.hpp>
#include <arpa/inet.h>

namespace trade_bot {

using boost::asio::ip::udp;

ClockDriftMonitor::ClockDriftMonitor(const Config& config)
    : config_(config) {}

ClockDriftMonitor::~ClockDriftMonitor() {
    stop();
}

void ClockDriftMonitor::start() {
    if (running_) return;
    running_ = true;
    thread_ = std::thread(&ClockDriftMonitor::run, this);
}

void ClockDriftMonitor::stop() {
    running_ = false;
    if (thread_.joinable()) {
        thread_.join();
    }
}

int64_t ClockDriftMonitor::drift_ms() const {
    std::lock_guard lock(mutex_);
    return static_cast<int64_t>(accumulator_.mean());
}

void ClockDriftMonitor::run() {
    while (running_) {
        auto drift = fetch_drift();
        if (drift) {
            // T0-CLOCK: check against average BEFORE update for 1-poll WARN, 2-poll Trigger
            double avg_before = accumulator_.mean();
            int64_t current_drift = std::abs(*drift);
            
            // Check Trigger against average drift (after at least 1 update)
            if (accumulator_.count() > 0 && std::abs(avg_before) >= config_.max_clock_drift_ms) {
                LOG_CRITICAL("Clock drift average {} ms exceeds maximum {} ms!", 
                    static_cast<int64_t>(avg_before), config_.max_clock_drift_ms);
                KillSwitch::trigger(KillReason::ClockDrift);
            }
            // Check WARN against sample
            else if (current_drift >= config_.warn_drift_ms) {
                LOG_WARN("Clock drift {} ms exceeds warning threshold {} ms", *drift, config_.warn_drift_ms);
            } else {
                LOG_DEBUG("Clock drift: {} ms", *drift);
            }
            
            // Update accumulator AFTER checks (Welford update)
            {
                std::lock_guard lock(mutex_);
                accumulator_.update(static_cast<double>(*drift));
            }
        }

        // Sleep for check_interval_sec, but check running_ flag
        for (int i = 0; i < config_.check_interval_sec * 10 && running_; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
}

std::optional<int64_t> ClockDriftMonitor::fetch_drift() {
    for (const auto& source : config_.sources) {
        auto drift = query_ntp(source);
        if (drift) return drift;
    }
    LOG_ERROR("Failed to fetch NTP time from all sources");
    return std::nullopt;
}

namespace {
    uint64_t ntp_to_unix_ms(const uint8_t* buffer) {
        uint32_t seconds;
        uint32_t fraction;
        std::memcpy(&seconds, buffer, 4);
        std::memcpy(&fraction, buffer + 4, 4);
        
        seconds = ntohl(seconds);
        fraction = ntohl(fraction);
        
        // NTP epoch is 1900-01-01, Unix epoch is 1970-01-01
        // Difference is 70 years, which is 2208988800 seconds
        const uint32_t NTP_UNIX_DIFF = 2208988800U;
        
        uint64_t unix_sec = seconds - NTP_UNIX_DIFF;
        uint64_t ms = (static_cast<uint64_t>(fraction) * 1000) >> 32;
        
        return unix_sec * 1000 + ms;
    }
    
    uint64_t current_unix_ms() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count();
    }
}

std::optional<int64_t> ClockDriftMonitor::query_ntp(const std::string& host) {
    try {
        udp::resolver resolver(io_context_);
        std::string hostname = host;
        std::string port = "123";
        
        size_t colon_pos = host.find(':');
        if (colon_pos != std::string::npos) {
            hostname = host.substr(0, colon_pos);
            port = host.substr(colon_pos + 1);
        }
        
        auto endpoints = resolver.resolve(udp::v4(), hostname, port);
        udp::socket socket(io_context_);
        socket.open(udp::v4());
        
        // Set timeout
        struct timeval tv;
        tv.tv_sec = 2;
        tv.tv_usec = 0;
        setsockopt(socket.native_handle(), SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        uint8_t packet[48] = {0};
        packet[0] = 0x23; // LI = 0, VN = 4, Mode = 3 (client)

        uint64_t t0 = current_unix_ms();
        socket.send_to(boost::asio::buffer(packet), *endpoints.begin());

        udp::endpoint sender_endpoint;
        size_t len = socket.receive_from(boost::asio::buffer(packet), sender_endpoint);
        uint64_t t3 = current_unix_ms();

        if (len < 48) return std::nullopt;

        uint64_t t1 = ntp_to_unix_ms(&packet[32]);
        uint64_t t2 = ntp_to_unix_ms(&packet[40]);

        // Drift formula: ((T1 - T0) + (T2 - T3)) / 2
        int64_t drift = (static_cast<int64_t>(t1) - static_cast<int64_t>(t0) + 
                         static_cast<int64_t>(t2) - static_cast<int64_t>(t3)) / 2;
        
        return drift;
    } catch (const std::exception& e) {
        LOG_WARN("NTP query to {} failed: {}", host, e.what());
        return std::nullopt;
    }
}

} // namespace trade_bot
