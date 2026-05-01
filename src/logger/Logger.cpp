#include "Logger.hpp"
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/daily_file_sink.h>
#include <vector>

namespace trade_bot {

std::shared_ptr<spdlog::logger> Logger::s_logger;

void Logger::init(const std::string& log_path) {
    auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    // "Beautiful" console format: colored level, short time, source:line
    console_sink->set_pattern("%^[%Y-%m-%d %H:%M:%S.%e] [%l] [%s:%#]%$ %v");

    auto file_sink = std::make_shared<spdlog::sinks::daily_file_sink_mt>(log_path, 0, 0);
    // "Structured" file format: ISO8601, full level, source:line
    file_sink->set_pattern("[%Y-%m-%dT%H:%M:%S.%fZ] [%l] [%s:%#] %v");

    std::vector<spdlog::sink_ptr> sinks { console_sink, file_sink };
    
    s_logger = std::make_shared<spdlog::logger>("trade_bot", sinks.begin(), sinks.end());
    
    #ifdef NDEBUG
        s_logger->set_level(spdlog::level::info);
    #else
        s_logger->set_level(spdlog::level::trace);
    #endif

    s_logger->flush_on(spdlog::level::warn);
    spdlog::set_default_logger(s_logger);
}

} // namespace trade_bot
