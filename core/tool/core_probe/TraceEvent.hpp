#pragma once

#include "domain/Ticker.hpp"
#include <string>
#include <cstdint>
#include <nlohmann/json.hpp>

namespace trade_bot::probe {

struct TraceEvent {
    uint64_t ts_ns = 0;
    uint64_t trace_id = 0;
    std::string ticker;
    std::string stage;
    std::string severity = "info"; // "info", "warn", "error"
    std::string message;
    nlohmann::json payload; // Structured fields for machine mode JSONL
};

} // namespace trade_bot::probe
