#pragma once

#include "CliOptions.hpp"
#include "TraceLogger.hpp"
#include "features/FeatureFrame.hpp"
#include "signals/Signal.hpp"
#include "strategy/TradePlan.hpp"

#include <atomic>
#include <cmath>
#include <set>
#include <string>

namespace trade_bot::probe {

class SummaryCollector;

/// Checks pipeline invariants at hot points and logs violations via TraceLogger.
/// Three modes:
///   "off"    — no checks at all
///   "on"     — check and warn (severity="warn"), execution continues
///   "strict" — check and error (severity="error"), caller may abort
class InvariantChecker {
public:
    explicit InvariantChecker(const CliOptions& opts, SummaryCollector* summary = nullptr);

    // ── Check groups ─────────────────────────────────────────────────────────

    void check_book(uint64_t trace_id,
                    const std::string& ticker,
                    double best_bid,
                    double best_ask,
                    double mid,
                    double spread_bps);

    void check_features(uint64_t trace_id,
                        const std::string& ticker,
                        const trade_bot::FeatureFrame& frame);

    void check_signal(uint64_t trace_id,
                      const trade_bot::Signal& sig);

    void check_plan(uint64_t trace_id,
                    const trade_bot::TradePlan& plan);

    void check_risk(uint64_t trace_id,
                    const std::string& ticker,
                    bool accepted,
                    const std::string& reason);

    void check_executor_close(uint64_t trace_id,
                              const std::string& ticker,
                              const std::set<std::string>& already_closed);

    void check_account(uint64_t trace_id,
                       double equity_usd);

    // ── Accessors ────────────────────────────────────────────────────────────

    [[nodiscard]] bool is_strict() const noexcept { return strict_; }
    [[nodiscard]] bool is_enabled() const noexcept { return enabled_; }
    [[nodiscard]] bool should_check_group(const std::string& group) const;
    [[nodiscard]] bool had_violation() const noexcept {
        return violation_count_.load(std::memory_order_relaxed) > 0;
    }
    [[nodiscard]] uint64_t violation_count() const noexcept {
        return violation_count_.load(std::memory_order_relaxed);
    }

private:
    void emit(uint64_t trace_id,
              const std::string& ticker,
              const std::string& code,
              const std::string& msg,
              const nlohmann::json& details,
              const std::string& hint);

    bool enabled_{false};
    bool strict_{false};
    std::set<std::string> invariant_set_; // empty = all groups enabled
    std::atomic<uint64_t> violation_count_{0};
    SummaryCollector* summary_{nullptr};
};

} // namespace trade_bot::probe
