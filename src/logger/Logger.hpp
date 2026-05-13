#pragma once

#include <spdlog/spdlog.h>
#include <spdlog/fmt/ostr.h>
#include <spdlog/sinks/base_sink.h>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace trade_bot {

// Ring-buffer spdlog sink — keeps the last kCapacity formatted log lines.
// Exposed so the dashboard can serve recent logs without tailing the file.
class LogRingBuffer final : public spdlog::sinks::base_sink<std::mutex> {
public:
    static constexpr std::size_t kCapacity = 200;

    // Returns up to `n` most recent entries (oldest first).
    std::vector<std::string> recent(std::size_t n) const;

protected:
    void sink_it_(const spdlog::details::log_msg& msg) override;
    void flush_() override {}

private:
    std::deque<std::string> buf_;
};

class Logger {
public:
    static void init(const std::string& log_path = "logs/trade_bot.log", const std::string& level = "info");
    static std::shared_ptr<spdlog::logger> get() { return s_logger; }
    static std::shared_ptr<LogRingBuffer>  ring()  { return s_ring; }

private:
    static std::shared_ptr<spdlog::logger> s_logger;
    static std::shared_ptr<LogRingBuffer>  s_ring;
};

} // namespace trade_bot

#define LOG_TRACE(...)    SPDLOG_LOGGER_TRACE(trade_bot::Logger::get(), __VA_ARGS__)
#define LOG_DEBUG(...)    SPDLOG_LOGGER_DEBUG(trade_bot::Logger::get(), __VA_ARGS__)
#define LOG_INFO(...)     SPDLOG_LOGGER_INFO(trade_bot::Logger::get(), __VA_ARGS__)
#define LOG_WARN(...)     SPDLOG_LOGGER_WARN(trade_bot::Logger::get(), __VA_ARGS__)
#define LOG_ERROR(...)    SPDLOG_LOGGER_ERROR(trade_bot::Logger::get(), __VA_ARGS__)
#define LOG_CRITICAL(...) SPDLOG_LOGGER_CRITICAL(trade_bot::Logger::get(), __VA_ARGS__)
