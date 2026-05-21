#pragma once

#include <nlohmann/json.hpp>
#include <atomic>
#include <cstdint>
#include <fstream>
#include <mutex>
#include <string>

namespace trade_bot {

/**
 * T2-LABELING: Records raw MetaScalp WebSocket messages to NDJSON.
 *
 * Output format (one line per message):
 *   {"recv_ts_ns": <int64>, "message": <WS JSON object>}
 *
 * Thread-safe: record() may be called from the I/O thread while
 * start()/stop() are called from the HTTP handler thread.
 */
class DumpRecorder {
public:
    // Returns true if file was opened successfully.
    bool start(const std::string& path);
    void stop();

    bool        is_active() const noexcept { return active_.load(); }
    std::string path()      const;

    void record(const nlohmann::json& msg, int64_t recv_ts_ns);

private:
    mutable std::mutex mtx_;
    std::ofstream      out_;
    std::string        path_;
    std::atomic<bool>  active_{false};
    uint32_t           write_count_{0};
};

} // namespace trade_bot
