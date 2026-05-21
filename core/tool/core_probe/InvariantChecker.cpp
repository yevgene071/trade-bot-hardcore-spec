#include "InvariantChecker.hpp"
#include "SummaryCollector.hpp"

#include <algorithm>
#include <sstream>

namespace trade_bot::probe {

InvariantChecker::InvariantChecker(const CliOptions& opts, SummaryCollector* summary)
    : enabled_(opts.invariants_mode != "off")
    , strict_(opts.invariants_mode == "strict")
    , invariant_set_(opts.invariant_set.begin(), opts.invariant_set.end())
    , summary_(summary)
{
}

// ── Private helpers ──────────────────────────────────────────────────────────

void InvariantChecker::emit(uint64_t trace_id,
                            const std::string& ticker,
                            const std::string& code,
                            const std::string& msg,
                            const nlohmann::json& details,
                            const std::string& hint) {
    violation_count_.fetch_add(1, std::memory_order_relaxed);

    if (summary_) {
        summary_->record_invariant(code);
    }

    const std::string severity = strict_ ? "error" : "warn";
    TraceLogger::instance().log_invariant(trace_id, ticker, severity, code, msg, details, hint);
}

bool InvariantChecker::should_check_group(const std::string& group) const {
    if (!enabled_) return false;
    // Empty set means "check everything"
    if (invariant_set_.empty()) return true;
    return invariant_set_.contains(group);
}

// ── Book checks ──────────────────────────────────────────────────────────────

void InvariantChecker::check_book(uint64_t trace_id,
                                  const std::string& ticker,
                                  double best_bid,
                                  double best_ask,
                                  double mid,
                                  double spread_bps) {
    if (!should_check_group("book")) return;

    // BOOK_CROSSED: best_bid >= best_ask means a crossed or locked book
    if (best_bid > 0.0 && best_ask > 0.0 && best_bid >= best_ask) {
        emit(trace_id, ticker, "BOOK_CROSSED",
             "Crossed book: best_bid >= best_ask",
             {{"best_bid", best_bid}, {"best_ask", best_ask}},
             "Check OrderBook::apply_update — best_bid >= best_ask after apply for " + ticker);
    }

    // BOOK_MID_INVALID: mid must be positive when both sides are present
    if (best_bid > 0.0 && best_ask > 0.0 && mid <= 0.0) {
        emit(trace_id, ticker, "BOOK_MID_INVALID",
             "Invalid mid price <= 0 with non-empty book",
             {{"mid", mid}, {"best_bid", best_bid}, {"best_ask", best_ask}},
             "Check OrderBook or FeatureExtractor mid calculation — mid <= 0 for " + ticker);
    }

    // BOOK_SPREAD_HUGE: unreasonable spread indicates stale or malformed data
    if (spread_bps > 1000.0) {
        emit(trace_id, ticker, "BOOK_SPREAD_HUGE",
             "Spread exceeds 1000 bps (" + std::to_string(spread_bps) + " bps)",
             {{"spread_bps", spread_bps}, {"best_bid", best_bid}, {"best_ask", best_ask}},
             "Check market data feed — spread > 1000 bps for " + ticker + ", possible stale side");
    }
}

// ── Feature checks ───────────────────────────────────────────────────────────

namespace {

/// Helper: check a single double field for NaN/Inf and emit if found.
[[maybe_unused]] bool check_field(InvariantChecker& self,
                 uint64_t trace_id,
                 const std::string& ticker,
                 const char* field_name,
                 double value) {
    // We only need to detect; emit is called externally via a lambda trick.
    return std::isnan(value) || std::isinf(value);
    (void)self;
    (void)trace_id;
    (void)ticker;
    (void)field_name;
}

} // namespace

void InvariantChecker::check_features(uint64_t trace_id,
                                      const std::string& ticker,
                                      const trade_bot::FeatureFrame& frame) {
    if (!should_check_group("features")) return;

    // Check key numerical fields for NaN/Inf
    struct FieldCheck {
        const char* name;
        double value;
    };

    const FieldCheck fields[] = {
        {"mid",                 frame.mid},
        {"spread_bps",          frame.spread_bps},
        {"imbalance",           frame.imbalance},
        {"buy_vol_1s",          frame.buy_vol_1s},
        {"sell_vol_1s",         frame.sell_vol_1s},
        {"volatility_1min_bps", frame.volatility_1min_bps},
        {"leader_correlation",  frame.leader_correlation},
    };

    for (const auto& [name, value] : fields) {
        if (std::isnan(value) || std::isinf(value)) {
            emit(trace_id, ticker, "FEATURES_NAN",
                 std::string("NaN/Inf detected in field '") + name + "'",
                 {{"field", name}, {"value", std::isnan(value) ? "NaN" : "Inf"}},
                 "Check FeatureExtractor::extract — NaN detected in field '" +
                     std::string(name) + "' for " + ticker);
        }
    }
}

// ── Signal checks ────────────────────────────────────────────────────────────

namespace {

/// Signal kinds that require a valid (positive) price anchor.
bool signal_needs_price(trade_bot::SignalKind kind) {
    switch (kind) {
        case trade_bot::SignalKind::DensityDetected:
        case trade_bot::SignalKind::DensityRemoved:
        case trade_bot::SignalKind::DensityEating:
        case trade_bot::SignalKind::LevelFormed:
        case trade_bot::SignalKind::LevelApproach:
        case trade_bot::SignalKind::LevelRejection:
        case trade_bot::SignalKind::LevelBreak:
            return true;
        default:
            return false;
    }
}

} // namespace

void InvariantChecker::check_signal(uint64_t trace_id,
                                    const trade_bot::Signal& sig) {
    if (!should_check_group("signal")) return;

    const auto& ticker = sig.ticker;

    // SIGNAL_CONFIDENCE_OOR: confidence must be in [0, 1]
    if (sig.confidence < 0.0 || sig.confidence > 1.0) {
        emit(trace_id, ticker, "SIGNAL_CONFIDENCE_OOR",
             "Signal confidence out of [0, 1] range: " + std::to_string(sig.confidence),
             {{"kind", static_cast<int>(sig.kind)}, {"confidence", sig.confidence}},
             "Check detector confidence calculation — value must be clamped to [0, 1]");
    }

    // SIGNAL_PRICE_INVALID: certain signal kinds must have a positive price
    if (signal_needs_price(sig.kind) && sig.price <= 0.0) {
        emit(trace_id, ticker, "SIGNAL_PRICE_INVALID",
             "Signal price <= 0 for kind that requires a price anchor",
             {{"kind", static_cast<int>(sig.kind)}, {"price", sig.price}},
             "Check detector price assignment — signal of this kind must have price > 0");
    }

    // SIGNAL_NO_TRACE: missing trace provenance makes e2e latency tracking impossible
    if (sig.trigger_trace_id == 0) {
        emit(trace_id, ticker, "SIGNAL_NO_TRACE",
             "Signal has trigger_trace_id == 0 (no provenance)",
             {{"kind", static_cast<int>(sig.kind)}},
             "Check detector emit path — pass TraceId from FeatureFrame::derived_from or book event");
    }
}

// ── TradePlan checks ─────────────────────────────────────────────────────────

void InvariantChecker::check_plan(uint64_t trace_id,
                                  const trade_bot::TradePlan& plan) {
    if (!should_check_group("plan")) return;

    const auto& ticker = plan.ticker;
    const auto side = plan.side;
    const double entry = plan.entry_price;
    const double stop = plan.stop_price;
    const double tp1 = plan.tp1_price;

    // PLAN_PRICE_INVALID: entry, stop, TP must all be positive
    if (entry <= 0.0) {
        emit(trace_id, ticker, "PLAN_PRICE_INVALID",
             "TradePlan entry_price <= 0",
             {{"entry_price", entry}},
             "Check strategy plan construction — entry_price must be > 0");
    }
    if (stop <= 0.0) {
        emit(trace_id, ticker, "PLAN_PRICE_INVALID",
             "TradePlan stop_price <= 0",
             {{"stop_price", stop}},
             "Check strategy stop calculation — stop_price must be > 0");
    }
    if (tp1 <= 0.0) {
        emit(trace_id, ticker, "PLAN_PRICE_INVALID",
             "TradePlan tp1_price <= 0",
             {{"tp1_price", tp1}},
             "Check strategy TP calculation — tp1_price must be > 0");
    }

    // PLAN_STOP_WRONG_SIDE: stop must be below entry for Buy, above for Sell
    if (entry > 0.0 && stop > 0.0) {
        if (side == trade_bot::Side::Buy && stop >= entry) {
            emit(trace_id, ticker, "PLAN_STOP_WRONG_SIDE",
                 "Stop >= entry for Buy trade",
                 {{"side", "Buy"}, {"entry_price", entry}, {"stop_price", stop}},
                 "Check strategy stop calculation — stop is on wrong side of entry for Buy trade");
        }
        if (side == trade_bot::Side::Sell && stop <= entry) {
            emit(trace_id, ticker, "PLAN_STOP_WRONG_SIDE",
                 "Stop <= entry for Sell trade",
                 {{"side", "Sell"}, {"entry_price", entry}, {"stop_price", stop}},
                 "Check strategy stop calculation — stop is on wrong side of entry for Sell trade");
        }
    }

    // PLAN_TP_WRONG_SIDE: TP must be above entry for Buy, below for Sell
    if (entry > 0.0 && tp1 > 0.0) {
        if (side == trade_bot::Side::Buy && tp1 <= entry) {
            emit(trace_id, ticker, "PLAN_TP_WRONG_SIDE",
                 "TP1 <= entry for Buy trade",
                 {{"side", "Buy"}, {"entry_price", entry}, {"tp1_price", tp1}},
                 "Check strategy TP calculation — TP is on wrong side of entry for Buy trade");
        }
        if (side == trade_bot::Side::Sell && tp1 >= entry) {
            emit(trace_id, ticker, "PLAN_TP_WRONG_SIDE",
                 "TP1 >= entry for Sell trade",
                 {{"side", "Sell"}, {"entry_price", entry}, {"tp1_price", tp1}},
                 "Check strategy TP calculation — TP is on wrong side of entry for Sell trade");
        }
    }

    // PLAN_NO_TRACE: missing trace provenance
    if (plan.trace_id == 0) {
        emit(trace_id, ticker, "PLAN_NO_TRACE",
             "TradePlan has trace_id == 0 (no provenance)",
             {},
             "Check strategy emit path — pass TraceId from Signal::trigger_trace_id");
    }
}

// ── Risk decision checks ─────────────────────────────────────────────────────

void InvariantChecker::check_risk(uint64_t trace_id,
                                  const std::string& ticker,
                                  bool accepted,
                                  const std::string& reason) {
    if (!should_check_group("risk")) return;

    // RISK_DECISION_INCONSISTENT: if accepted, reason should be "None" or empty
    if (accepted && !reason.empty() && reason != "None") {
        emit(trace_id, ticker, "RISK_DECISION_INCONSISTENT",
             "Risk accepted=true but reject reason is '" + reason + "'",
             {{"accepted", accepted}, {"reason", reason}},
             "Check RiskManager::evaluate — accepted trades must have reason=None, got '" + reason + "'");
    }
}

// ── Executor checks ──────────────────────────────────────────────────────────

void InvariantChecker::check_executor_close(uint64_t trace_id,
                                            const std::string& ticker,
                                            const std::set<std::string>& already_closed) {
    if (!should_check_group("executor")) return;

    // EXECUTOR_DOUBLE_CLOSE: ticker already in closed set
    if (already_closed.contains(ticker)) {
        emit(trace_id, ticker, "EXECUTOR_DOUBLE_CLOSE",
             "Attempting to close position for '" + ticker + "' that is already closed",
             {{"ticker", ticker}},
             "Check PaperExecutor/LiveExecutor close path — duplicate close for " + ticker);
    }
}

// ── Account checks ───────────────────────────────────────────────────────────

void InvariantChecker::check_account(uint64_t trace_id,
                                     double equity_usd) {
    if (!should_check_group("account")) return;

    // ACCOUNT_NEGATIVE_EQUITY: equity must never go negative in a sane simulation
    if (equity_usd < 0.0) {
        emit(trace_id, "", "ACCOUNT_NEGATIVE_EQUITY",
             "Account equity is negative: " + std::to_string(equity_usd) + " USD",
             {{"equity_usd", equity_usd}},
             "Check AccountState or PaperExecutor PnL accounting — equity < 0 indicates a bug or missing margin call");
    }
}

} // namespace trade_bot::probe
