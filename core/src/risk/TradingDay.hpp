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
        std::tm tm_buf;
#if defined(_WIN32)
        gmtime_s(&tm_buf, &in_time_t);
#else
        gmtime_r(&in_time_t, &tm_buf);
#endif
        tm_buf.tm_hour = 0;
        tm_buf.tm_min = 0;
        tm_buf.tm_sec = 0;
        
        auto time_utc = [](std::tm* tm_ptr) {
#if defined(_WIN32)
            return _mkgmtime(tm_ptr);
#else
            return timegm(tm_ptr);
#endif
        };
        
        return std::chrono::system_clock::from_time_t(time_utc(&tm_buf));
    }
    
    static bool is_new_day(const std::string& last_reset_day_utc) {
        return last_reset_day_utc != current_date_utc();
    }
};

} // namespace trade_bot
