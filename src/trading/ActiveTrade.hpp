#pragma once

#include "strategy/TradePlan.hpp"
#include <chrono>

namespace trade_bot {

enum class TradeState {
    Submitted,
    Open,
    TP1_Hit,
    Recovered,
    Closed
};

struct ActiveTrade {
    TradePlan plan;
    int       entry_order_id;
    TradeState state;
    double    executed_size;
    double    avg_entry_price;
    std::chrono::system_clock::time_point opened_at;
    
    bool operator==(const ActiveTrade&) const = default;
};

} // namespace trade_bot
