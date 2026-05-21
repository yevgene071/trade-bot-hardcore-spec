#pragma once

#include <nlohmann/json.hpp>
#include <map>
#include <string>
#include <cstdint>
#include <sstream>

namespace trade_bot::probe {

/// Accumulates statistics across a full core_probe pipeline run.
/// Not thread-safe — expected to be called from a single orchestration thread.
class SummaryCollector {
public:
    // --- Recording methods (call during pipeline execution) ---

    void record_message() noexcept { ++messages_parsed_; }
    void record_parse_error() noexcept { ++parse_errors_; }
    void record_dropped() noexcept { ++dropped_; }

    void record_signal(const std::string& kind);
    void record_plan(const std::string& strategy_name);
    void record_risk_decision(bool accepted, const std::string& reason);
    void record_submit() noexcept { ++submitted_; }
    void record_close(double pnl_usd);
    /// Overload: also tracks loss/win disposition.  `is_loss` is currently
    /// informational; per-disposition aggregates can be added when needed.
    void record_close(double pnl_usd, bool /*is_loss*/) { record_close(pnl_usd); }
    void record_invariant(const std::string& code);

    // --- Setters for post-run metadata ---

    void set_start_time(uint64_t ts_ns) noexcept { start_ts_ns_ = ts_ns; }
    void set_end_time(uint64_t ts_ns) noexcept { end_ts_ns_ = ts_ns; }
    void set_market_duration_sec(double sec) noexcept { market_duration_sec_ = sec; }
    void set_wall_duration_sec(double sec) noexcept { wall_duration_sec_ = sec; }
    void set_unrealized_pnl(double pnl) noexcept { unrealized_pnl_ = pnl; }
    void set_source_description(const std::string& desc) { source_description_ = desc; }
    void set_source(const std::string& desc) { source_description_ = desc; }
    void set_open_at_end(uint64_t n) noexcept { open_at_end_ = n; }

    // --- Output ---

    /// Machine-readable JSON summary for JSONL output.
    nlohmann::json to_json() const;

    /// Human-readable summary block for terminal output.
    std::string to_human_string() const;

    // --- Accessors (for assertions / tests) ---

    uint64_t messages_parsed() const noexcept { return messages_parsed_; }
    uint64_t parse_errors() const noexcept { return parse_errors_; }
    uint64_t dropped() const noexcept { return dropped_; }
    uint64_t plans_accepted() const noexcept { return plans_accepted_; }
    uint64_t plans_rejected() const noexcept { return plans_rejected_; }
    uint64_t submitted() const noexcept { return submitted_; }
    uint64_t closed() const noexcept { return closed_; }
    uint64_t open_at_end() const noexcept { return open_at_end_; }
    double realized_pnl() const noexcept { return realized_pnl_; }
    double unrealized_pnl() const noexcept { return unrealized_pnl_; }
    uint64_t invariant_total() const noexcept { return invariant_total_; }

    /// Aggregated counters used by finalize-summary trace event.
    uint64_t total_signals() const noexcept {
        uint64_t n = 0;
        for (const auto& [_, c] : signals_by_kind_) n += c;
        return n;
    }
    uint64_t total_plans() const noexcept {
        uint64_t n = 0;
        for (const auto& [_, c] : plans_by_strategy_) n += c;
        return n;
    }
    uint64_t total_submits() const noexcept { return submitted_; }
    uint64_t total_closes() const noexcept { return closed_; }

    const std::map<std::string, uint64_t>& signals_by_kind() const noexcept { return signals_by_kind_; }
    const std::map<std::string, uint64_t>& plans_by_strategy() const noexcept { return plans_by_strategy_; }
    const std::map<std::string, uint64_t>& rejections_by_reason() const noexcept { return rejections_by_reason_; }
    const std::map<std::string, uint64_t>& invariants_by_code() const noexcept { return invariants_by_code_; }

private:
    // Source
    std::string source_description_;

    // Messages
    uint64_t messages_parsed_ = 0;
    uint64_t parse_errors_ = 0;
    uint64_t dropped_ = 0;

    // Signals
    std::map<std::string, uint64_t> signals_by_kind_;

    // Plans
    std::map<std::string, uint64_t> plans_by_strategy_;
    uint64_t plans_accepted_ = 0;
    uint64_t plans_rejected_ = 0;
    std::map<std::string, uint64_t> rejections_by_reason_;

    // Executor
    uint64_t submitted_ = 0;
    uint64_t closed_ = 0;
    uint64_t open_at_end_ = 0;
    double realized_pnl_ = 0.0;
    double unrealized_pnl_ = 0.0;

    // Invariants
    uint64_t invariant_total_ = 0;
    std::map<std::string, uint64_t> invariants_by_code_;

    // Timing
    uint64_t start_ts_ns_ = 0;
    uint64_t end_ts_ns_ = 0;
    double market_duration_sec_ = 0.0;
    double wall_duration_sec_ = 0.0;
};

} // namespace trade_bot::probe
