#include "TraceFormatter.hpp"
#include <iomanip>
#include <sstream>
#include <chrono>

namespace trade_bot::probe {

bool TraceFormatter::include_payload_ = true; // include by default

std::string TraceFormatter::format_timestamp(uint64_t ts_ns) {
    if (ts_ns == 0) {
        return "00:00:00.000000";
    }

    // Use std::chrono arithmetic — avoid localtime_r, no C library dependency.
    using namespace std::chrono;
    auto dur = nanoseconds(ts_ns);

    auto hh = duration_cast<hours>(dur) % 24;
    auto mm = duration_cast<minutes>(dur) % 60;
    auto ss = duration_cast<seconds>(dur) % 60;
    uint64_t micros = (ts_ns % 1'000'000'000) / 1000;

    char buf[64];
    std::snprintf(buf, sizeof(buf), "%02ld:%02ld:%02ld.%06lu",
                  static_cast<long>(hh.count()),
                  static_cast<long>(mm.count()),
                  static_cast<long>(ss.count()),
                  micros);
    return std::string(buf);
}

std::string TraceFormatter::get_ansi_color(const std::string& stage) {
    // Return appropriate colors depending on the stage
    if (stage == "ws_raw") return "\033[90m";     // Gray
    if (stage == "parse") return "\033[36m";      // Cyan
    if (stage == "book") return "\033[34m";       // Blue
    if (stage == "trades") return "\033[94m";     // Light Blue
    if (stage == "features") return "\033[35m";   // Magenta
    if (stage == "signal") return "\033[33m";     // Yellow
    if (stage == "strategy") return "\033[32m";   // Green
    if (stage == "risk") return "\033[91m";       // Light Red
    if (stage == "executor") return "\033[31m";   // Red
    if (stage == "account") return "\033[92m";    // Light Green
    if (stage == "invariant") return "\033[93m";  // Bright Yellow / Orange
    if (stage == "error") return "\033[41m\033[37m"; // White on Red background
    if (stage == "perf") return "\033[96m";       // Light Cyan
    if (stage == "summary") return "\033[1m\033[32m"; // Bold Green
    return "";
}

std::string TraceFormatter::to_human_string(const TraceEvent& ev, bool no_color) {
    std::string color_start = no_color ? "" : get_ansi_color(ev.stage);
    std::string color_end = no_color ? "" : "\033[0m";
    std::string reset_bold = no_color ? "" : "\033[0m";
    
    char trace_buf[32];
    std::snprintf(trace_buf, sizeof(trace_buf), "%07lu", ev.trace_id);
    
    std::ostringstream ss;
    ss << "[" << format_timestamp(ev.ts_ns) << "] "
       << "[trace=" << trace_buf << "] "
       << "[" << std::left << std::setw(8) << ev.ticker << "] "
       << color_start << "[" << std::left << std::setw(9) << ev.stage << "]" << color_end << " "
       << ev.message;
       
    return ss.str();
}

std::string TraceFormatter::to_machine_string(const TraceEvent& ev) {
    nlohmann::json j;
    j["stage"] = ev.stage;
    j["ts_ns"] = ev.ts_ns;
    j["trace_id"] = ev.trace_id;
    if (!ev.ticker.empty()) j["ticker"] = ev.ticker;
    if (ev.severity != "info") j["severity"] = ev.severity;

    // Include message (needed for stages that only have message, not payload)
    if (!ev.message.empty() && !j.contains("message")) {
        j["message"] = ev.message;
    }

    // Merge payload only if include_payload_ is enabled
    if (include_payload_ && ev.payload.is_object()) {
        for (auto it = ev.payload.begin(); it != ev.payload.end(); ++it) {
            j[it.key()] = it.value();
        }
    }

    return j.dump();
}

} // namespace trade_bot::probe
