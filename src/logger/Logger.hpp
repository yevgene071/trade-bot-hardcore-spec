#pragma once

#include <spdlog/spdlog.h>
#include <spdlog/fmt/ostr.h>
#include <memory>
#include <string>

namespace trade_bot {

class Logger {
public:
    static void init(const std::string& log_path = "logs/trade_bot.log");
    static std::shared_ptr<spdlog::logger> get() { return s_logger; }

private:
    static std::shared_ptr<spdlog::logger> s_logger;
};

} // namespace trade_bot

#define LOG_TRACE(...)    SPDLOG_LOGGER_TRACE(trade_bot::Logger::get(), __VA_ARGS__)
#define LOG_DEBUG(...)    SPDLOG_LOGGER_DEBUG(trade_bot::Logger::get(), __VA_ARGS__)
#define LOG_INFO(...)     SPDLOG_LOGGER_INFO(trade_bot::Logger::get(), __VA_ARGS__)
#define LOG_WARN(...)     SPDLOG_LOGGER_WARN(trade_bot::Logger::get(), __VA_ARGS__)
#define LOG_ERROR(...)    SPDLOG_LOGGER_ERROR(trade_bot::Logger::get(), __VA_ARGS__)
#define LOG_CRITICAL(...) SPDLOG_LOGGER_CRITICAL(trade_bot::Logger::get(), __VA_ARGS__)
