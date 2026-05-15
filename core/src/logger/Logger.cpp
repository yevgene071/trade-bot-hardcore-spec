#include "Logger.hpp"
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/daily_file_sink.h>
#include <spdlog/details/log_msg.h>
#include <vector>

namespace trade_bot {

std::shared_ptr<spdlog::logger> Logger::s_logger;
std::shared_ptr<LogRingBuffer>  Logger::s_ring;

void LogRingBuffer::sink_it_(const spdlog::details::log_msg& msg) {
    spdlog::memory_buf_t buf;
    formatter_->format(msg, buf);
    std::string line = fmt::to_string(buf);
    // Strip trailing newline
    if (!line.empty() && line.back() == '\n') line.pop_back();
    buf_.push_back(std::move(line));
    if (buf_.size() > kCapacity) buf_.pop_front();
}

std::vector<std::string> LogRingBuffer::recent(std::size_t n) const {
    std::lock_guard lk(const_cast<std::mutex&>(mutex_));
    std::size_t start = buf_.size() > n ? buf_.size() - n : 0;
    return {buf_.begin() + static_cast<std::ptrdiff_t>(start), buf_.end()};
}

void Logger::init(const std::string& log_path, const std::string& level) {
    auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    console_sink->set_pattern("%^[%Y-%m-%d %H:%M:%S.%e] [%l] [%s:%#]%$ %v");

    auto file_sink = std::make_shared<spdlog::sinks::daily_file_sink_mt>(log_path, 0, 0);
    file_sink->set_pattern("[%Y-%m-%dT%H:%M:%S.%fZ] [%l] [%s:%#] %v");

    s_ring = std::make_shared<LogRingBuffer>();
    s_ring->set_pattern("[%H:%M:%S.%e] [%l] [%s:%#] %v");

    std::vector<spdlog::sink_ptr> sinks { console_sink, file_sink, s_ring };

    s_logger = std::make_shared<spdlog::logger>("trade_bot", sinks.begin(), sinks.end());

    spdlog::level::level_enum lvl = spdlog::level::info;
    if (level == "trace") lvl = spdlog::level::trace;
    else if (level == "debug") lvl = spdlog::level::debug;
    else if (level == "warn") lvl = spdlog::level::warn;
    else if (level == "error") lvl = spdlog::level::err;
    else if (level == "critical") lvl = spdlog::level::critical;
    
    s_logger->set_level(lvl);
    s_logger->flush_on(spdlog::level::warn);
    spdlog::set_default_logger(s_logger);
}

} // namespace trade_bot
