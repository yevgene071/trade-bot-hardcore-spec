#include "SchemaRunner.hpp"

#include <iostream>
#include <nlohmann/json.hpp>

namespace trade_bot::probe {

int SchemaRunner::run() {
    using json = nlohmann::json;

    json schema;
    schema["schema_version"] = "1.0";
    schema["description"] = "core_probe trace event schema";

    // Common fields present in every event
    json common;
    common["stage"]    = "string — stage name (see stages below)";
    common["ts_ns"]    = "uint64 — nanosecond timestamp (system clock)";
    common["trace_id"] = "uint64 — monotonic trace ID linking events in one pipeline pass";
    common["ticker"]   = "string — ticker symbol (e.g. BTC_USDT); omitted for meta/summary";
    schema["common_fields"] = common;

    json stages;

    // meta
    {
        json s;
        s["description"] = "First event in machine mode: schema, version, config info";
        s["fields"] = {
            {"schema_version", "string"},
            {"core_probe_version", "string"},
            {"git_sha", "string"},
            {"config", "string — path to config file"},
            {"started_at_ns", "uint64"}
        };
        stages["meta"] = s;
    }

    // ws_raw
    {
        json s;
        s["description"] = "Raw WS message or NDJSON line (verbose mode only)";
        s["fields"] = {
            {"message", "string — raw message content or summary"},
            {"size_bytes", "uint64"}
        };
        stages["ws_raw"] = s;
    }

    // parse
    {
        json s;
        s["description"] = "MetaScalpCodec parse result";
        s["fields"] = {
            {"event_type", "string — e.g. orderbook_update, trade_update"},
            {"parse_time_us", "uint64"}
        };
        stages["parse"] = s;
    }

    // book
    {
        json s;
        s["description"] = "OrderBook state after apply_snapshot / apply_update";
        s["fields"] = {
            {"mid", "double — mid price"},
            {"spread_bps", "double — bid-ask spread in basis points"},
            {"imbalance", "double — depth imbalance [-1, 1]"},
            {"best_bid", "double"},
            {"best_ask", "double"},
            {"delta_levels", "int — number of levels changed"}
        };
        stages["book"] = s;
    }

    // trades
    {
        json s;
        s["description"] = "Trade print (or throttled summary)";
        s["fields"] = {
            {"price", "double"},
            {"size", "double"},
            {"side", "string — Buy or Sell"},
            {"count", "int — number of trades in batch (if throttled)"}
        };
        stages["trades"] = s;
    }

    // features
    {
        json s;
        s["description"] = "FeatureFrame snapshot after extraction";
        s["fields"] = {
            {"mid", "double"},
            {"spread_bps", "double"},
            {"imbalance", "double"},
            {"volatility_1min_bps", "double"},
            {"prints_per_sec", "double"},
            {"buy_vol_5s", "double"},
            {"sell_vol_5s", "double"},
            {"leader_correlation", "double"},
            {"leader_lag_ms", "double"},
            {"tape_aggression", "double"}
        };
        stages["features"] = s;
    }

    // signal
    {
        json s;
        s["description"] = "Signal published to SignalBus";
        s["fields"] = {
            {"kind", "string — DensityDetected, IcebergSuspected, TapeBurst, etc."},
            {"price", "double — price the signal is anchored to"},
            {"confidence", "double — [0, 1]"},
            {"side", "string — Bid, Ask, Buy, Sell"},
            {"size_usd", "double — relevant size in USD (if applicable)"}
        };
        stages["signal"] = s;
    }

    // strategy
    {
        json s;
        s["description"] = "Strategy emitted a TradePlan";
        s["fields"] = {
            {"strategy", "string — strategy name (e.g. BounceFromDensity)"},
            {"side", "string — Buy or Sell"},
            {"entry_price", "double"},
            {"stop_price", "double"},
            {"tp1_price", "double"},
            {"size_coin", "double"},
            {"risk_usd", "double"}
        };
        stages["strategy"] = s;
    }

    // risk
    {
        json s;
        s["description"] = "RiskManager decision on a TradePlan";
        s["fields"] = {
            {"accepted", "bool"},
            {"reason", "string — RejectReason name (None if accepted)"},
            {"details", "string — human-readable explanation"},
            {"adjusted_size_coin", "double — size after risk adjustment"}
        };
        stages["risk"] = s;
    }

    // executor
    {
        json s;
        s["description"] = "Order lifecycle event: submit, close";
        s["fields"] = {
            {"action", "string — submit, close"},
            {"entry_price", "double"},
            {"exit_price", "double — for close events"},
            {"size_filled", "double — for close events"},
            {"pnl_usd", "double — realized PnL for close events"},
            {"reason", "string — close reason"}
        };
        stages["executor"] = s;
    }

    // account
    {
        json s;
        s["description"] = "Account state change after trade close";
        s["fields"] = {
            {"equity_usd", "double"},
            {"realized_pnl_usd", "double"},
            {"unrealized_pnl_usd", "double"},
            {"active_positions", "int"}
        };
        stages["account"] = s;
    }

    // invariant
    {
        json s;
        s["description"] = "Invariant violation detected";
        s["fields"] = {
            {"severity", "string — warn or error"},
            {"code", "string — e.g. BOOK_CROSSED, FEATURES_NAN"},
            {"details", "object — violation-specific data"},
            {"hint", "string — where to look to fix the issue"}
        };
        stages["invariant"] = s;
    }

    // error
    {
        json s;
        s["description"] = "Unhandled exception or unexpected error";
        s["fields"] = {
            {"exception_type", "string"},
            {"message", "string"}
        };
        stages["error"] = s;
    }

    // perf
    {
        json s;
        s["description"] = "Per-stage latency histograms (in summary)";
        s["fields"] = {
            {"stage_name", "string"},
            {"p50_us", "int64"},
            {"p99_us", "int64"},
            {"max_us", "int64"},
            {"count", "int64"}
        };
        stages["perf"] = s;
    }

    // summary
    {
        json s;
        s["description"] = "Final aggregate: counts, PnL, latency";
        s["fields"] = {
            {"messages_parsed", "uint64"},
            {"messages_dropped", "uint64"},
            {"parse_errors", "uint64"},
            {"signals", "object — {kind: count} map"},
            {"plans", "object — generated/accepted/rejected counts"},
            {"invariants", "object — {total, by_code}"},
            {"executor", "object — submitted/closed/pnl"},
            {"latency_us", "object — per-stage {p50, p99, max}"},
            {"wall_duration_sec", "double"},
            {"market_duration_sec", "double"}
        };
        stages["summary"] = s;
    }

    schema["stages"] = stages;

    // Print with 2-space indent
    std::cout << schema.dump(2) << "\n";
    return 0;
}

} // namespace trade_bot::probe
