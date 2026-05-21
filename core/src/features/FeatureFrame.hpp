#pragma once

#include "domain/Types.hpp"
#include "perf/LatencyTracer.hpp"

#include <chrono>
#include <optional>

namespace trade_bot {

/**
 * Snapshot of all observed features at time T. Layout: AoS for now (single
 * frame at a time); a SoA batch container will land alongside the detector
 * pipeline in Phase 2 where we iterate row-wise across many tickers.
 *
 * All volume fields are float-typed for downstream consumption. The fixed-
 * point representation lives inside the source modules (TradeStream Kahan,
 * OrderBook PriceTick); we materialise to double here to keep the frame
 * compact and ABI-stable.
 */
inline constexpr double kCrossedBookSpreadBps = 999.0;

struct FeatureFrame {
    std::chrono::system_clock::time_point timestamp{};
    Ticker                                ticker;

    // Order book
    double best_bid{};
    double best_ask{};
    double mid{};
    double spread_abs{};
    double spread_bps{};
    double bid_depth_10{};
    double ask_depth_10{};
    double imbalance{};            // (bid_d10 - ask_d10) / (bid_d10 + ask_d10) ∈ [-1, 1]

    // Trade stream
    double buy_vol_1s{},  buy_vol_5s{},  buy_vol_30s{};
    double sell_vol_1s{}, sell_vol_5s{}, sell_vol_30s{};
    double prints_per_sec{};
    double avg_print_size{};
    double max_print_size_5s{};
    double tape_aggression{};      // (buy - sell) / total over the latest 5s window

    // Price dynamics
    double price_change_1s{};      // in %
    double price_change_2s{};
    double price_change_5s{};
    double price_change_30s{};
    double volatility_1min{};      // stdev of 1-min log returns (dimensionless)
    double volatility_1min_bps{};  // same in basis points

    // Leader
    double leader_change_1s{};
    double leader_change_5s{};
    double leader_correlation{};
    double leader_lag_ms{};

    // Level proximity (filled by Phase-2 LevelDetector — left at defaults here)
    std::optional<double> nearest_support;
    std::optional<double> nearest_resistance;
    double dist_to_support_bps{};
    double dist_to_resistance_bps{};

    // Exchange data (injected by BotApp tick loop from MarketDataFeed cache)
    double funding_rate{};   // 8h funding rate, raw decimal (e.g. 0.0001 = 1 bps); 0 if unknown
    double mark_price{};     // exchange mark price; 0 if unavailable

    // W3-FIX: validity flag — false until producer confirms mid > 0
    bool valid{false};

    // Performance tracing: trace ID of the event that triggered this frame extraction
    TraceId derived_from{0};
};

}  // namespace trade_bot
