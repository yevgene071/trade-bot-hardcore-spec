#pragma once

#include <chrono>
#include <optional>
#include <string>

namespace trade_bot {

/// Pluggable NTP/SNTP client. Implementations return the time reported by
/// `host` (NTP server) or std::nullopt on timeout/error. Allows
/// ClockDriftMonitor to be unit-tested with deterministic mocks.
class INtpClient {
public:
    virtual ~INtpClient() = default;

    /// Query `host` and return its current UTC time, or nullopt on
    /// timeout/error. `timeout_ms` is per-request budget.
    virtual std::optional<std::chrono::system_clock::time_point>
    query(const std::string& host, int timeout_ms) = 0;
};

}  // namespace trade_bot
