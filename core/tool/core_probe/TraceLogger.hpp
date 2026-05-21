#pragma once

#include "CliOptions.hpp"
#include "TraceEvent.hpp"
#include <concurrentqueue.h>
#include <thread>
#include <atomic>
#include <string>
#include <vector>
#include <fstream>
#include <memory>
#include <unordered_set>
#include <unordered_map>

namespace trade_bot::probe {

class TraceLogger {
public:
    static TraceLogger& instance() {
        static TraceLogger inst;
        return inst;
    }

    void init(const CliOptions& opts);
    void shutdown();

    void enqueue(TraceEvent&& ev) noexcept;

    // Direct logging helpers
    void log_meta(const std::string& schema_version, const std::string& git_sha, const std::string& config_path);
    void log_error(uint64_t trace_id, const std::string& ticker, const std::string& msg, const nlohmann::json& payload = {});
    void log_invariant(uint64_t trace_id, const std::string& ticker, const std::string& severity, const std::string& code, const std::string& msg, const nlohmann::json& details, const std::string& hint);

    uint64_t get_dropped_count() const noexcept { return dropped_count_.load(); }
    uint64_t get_total_count() const noexcept { return total_count_.load(); }

private:
    TraceLogger() = default;
    ~TraceLogger();

    void writer_loop();
    bool should_log(const TraceEvent& ev);

    CliOptions opts_;
    std::atomic<bool> running_{false};
    std::thread writer_thread_;

    moodycamel::ConcurrentQueue<TraceEvent> queue_;
    std::atomic<uint64_t> dropped_count_{0};
    std::atomic<uint64_t> total_count_{0};

    std::unique_ptr<std::ofstream> file_sink_;
    
    std::unordered_set<std::string> stage_filter_;
    std::unordered_set<std::string> mute_filter_;
    
    // Throttle tracker: ticker -> last log timestamp for book stage
    std::unordered_map<std::string, uint64_t> last_book_log_ns_;
};

} // namespace trade_bot::probe
