#pragma once

#include "domain/Ticker.hpp"
#include <string>
#include <vector>
#include <map>
#include <optional>

namespace trade_bot::probe {

enum class Subcommand {
    None,
    Replay,
    Live,
    Synth,
    Diff,
    Assert,
    Schema
};

struct CliOptions {
    Subcommand cmd = Subcommand::None;

    // Subcommand specifics
    std::string dump_path;
    std::vector<std::string> tickers;
    std::string ws_url = "wss://api.metascalp.io/v1/ws"; // default placeholder or similar
    std::string scenario;
    std::string left_path;
    std::string right_path;
    std::vector<std::string> expectations;

    // Common flags
    std::string config_path = "config.toml";
    std::vector<std::string> detectors;
    std::vector<std::string> strategies;
    bool no_strategy = false;
    bool no_risk = false;
    bool no_executor = false;
    bool risk_observe = false;

    // Filters & Logging
    std::vector<std::string> stages;
    std::vector<std::string> mute_stages;
    std::optional<uint64_t> trace_id_filter;
    uint32_t throttle_book_ms = 0;
    bool quiet = false;
    bool verbose = false;

    // Output
    bool machine = false;
    bool no_color = false;
    std::string jsonl_out;
    bool no_jsonl = false;
    bool no_stdout = false;
    bool include_payload = false;

    // AI & Invariants
    std::string ai_recipe;
    std::map<std::string, std::string> ai_recipe_args;
    std::string invariants_mode = "on"; // "on", "off", "strict"
    std::vector<std::string> invariant_set;

    // Feed / Engine settings
    double speed_multiplier = 0.0; // as-fast-as-possible by default
    uint64_t limit_messages = 0;
    std::string start_ts_iso;
    double equity_usd = 100000.0;

    // Helpers
    bool list_stages = false;
    bool show_version = false;

    static CliOptions parse(int argc, char* argv[]);
    void print_help() const;
};

} // namespace trade_bot::probe
