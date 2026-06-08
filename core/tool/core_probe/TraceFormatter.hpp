#pragma once

#include "TraceEvent.hpp"
#include <string>

namespace trade_bot::probe {

class TraceFormatter {
public:
    static std::string to_human_string(const TraceEvent& ev, bool no_color = false);
    static std::string to_machine_string(const TraceEvent& ev);

    /// Control whether payload fields are included in machine (JSONL) output.
    static void set_include_payload(bool v) { include_payload_ = v; }

    // Helpers
    static std::string get_ansi_color(const std::string& stage);
    static std::string format_timestamp(uint64_t ts_ns);

private:
    static bool include_payload_;
};

} // namespace trade_bot::probe
