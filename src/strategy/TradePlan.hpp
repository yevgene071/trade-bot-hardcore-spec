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
    Side side{Side::None};
    OrderType entry_type{OrderType::Limit};
    double entry_price{0.0};
    double stop_price{0.0};
    double tp1_price{0.0};
    std::optional<double> tp2_price;
    double tp1_size_ratio{0.0};
    double size_coin{0.0};
    double risk_usd{0.0};
    std::string strategy_name;
    std::string reason;
    std::vector<Signal> evidence;
    std::chrono::system_clock::time_point valid_until;

    bool operator==(const TradePlan&) const = default;
};

} // namespace trade_bot
