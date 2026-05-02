#pragma once

#include <chrono>
#include <string>

namespace trade_bot {

/**
 * T4-RISK: Helper for UTC-based trading day tracking and reset.
 */
class TradingDay {
public:
    static std::string current_date_utc() {
        auto now = std::chrono::system_clock::now();
        auto in_time_t = std::chrono::system_clock::to_time_t(now);
        char buf[16];
        std::strftime(buf, sizeof(buf), "%Y-%m-%d", std::gmtime(&in_time_t));
        return std::string(buf);
    }

    static std::chrono::system_clock::time_point start_of_day_utc() {
        auto now = std::chrono::system_clock::now();
        auto in_time_t = std::chrono::system_clock::to_time_t(now);
        auto* tm = std::gmtime(&in_time_t);
        tm->tm_hour = 0;
        tm->tm_min = 0;
        tm->tm_sec = 0;
#if defined(__linux__)
        return std::chrono::system_clock::from_time_t(timegm(tm));
#else
        return std::chrono::system_clock::from_time_t(mktime(tm)); // Not UTC-safe on non-Linux
#endif
    }
    
    static bool is_new_day(const std::string& last_reset_day_utc) {
        return last_reset_day_utc != current_date_utc();
    }
};

} // namespace trade_bot
