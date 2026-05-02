#pragma once

#include "domain/Types.hpp"
#include "signals/Signal.hpp"

#include <string>
#include <vector>
#include <optional>
#include <chrono>

namespace trade_bot {

/**
 * T3-PLAN: Proposed trade execution details from a strategy.
 */
struct TradePlan {
    Ticker ticker;
    Side side;                   // Buy / Sell
    OrderType entry_type;        // Limit / Market
    double entry_price;          // for Limit/StopLimit
    double stop_price;           // mandatory
    double tp1_price;            // mandatory: first target (>= 1R)
    std::optional<double> tp2_price;  // optional: far target
    double tp1_size_ratio;       // how much to close at TP1 (0.5-0.7)
    double size_coin;            // position size in coin
    double risk_usd;              // expected loss in $ if stop hits
    std::string strategy_name;
    std::string reason;           // human-readable "why we entered"
    std::vector<Signal> evidence; // signals that justified the entry
    std::chrono::system_clock::time_point valid_until;  // TTL
    
    bool operator==(const TradePlan&) const = default;
};

} // namespace trade_bot
