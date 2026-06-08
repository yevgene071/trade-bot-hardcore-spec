#pragma once

#include "strategy/TradePlan.hpp"
#include <chrono>

namespace trade_bot {

enum class TradeState {
    PendingEntry,
    Open,
    Exiting,
    Closed,
    Cancelling,
    SubmitUnknown,
    EmergencyClosing  // B4-FIX: Prevent race between emergency close and on_order_update
};

struct ActiveTrade {
    TradePlan  plan;
    // Server-assigned OrderIds, captured from the first WS order_update for
    // each role. 0 means "not yet observed". MetaScalp returns the id only
    // through the WS stream, not from POST /orders, so these are populated
    // asynchronously. Issue #129.
    int64_t    entry_order_id{0};
    int64_t    stop_order_id{0};
    // Local correlation key for the entry order. MetaScalp does not document
    // caller-supplied ClientId in POST /orders; this is not sent on the wire.
    // First-sight matching normally falls back to side+type+size until a
    // server OrderId is observed.
    std::string entry_client_order_id;
    int64_t    tp1_order_id{0};
    int64_t    tp2_order_id{0};
    TradeState state{TradeState::PendingEntry};
    double     executed_size{0.0};
    double     avg_entry_price{0.0};
    /// Weighted average of entry orders only (PositionUpdate.AvgPriceFix).
    /// Used to re-price the stop to break-even after TP1 per T4-EXECUTOR
    /// spec — explicitly NOT AvgPriceDyn, which is adjusted by realized
    /// exit PnL and would systematically mis-price the BE stop.
    /// Issue #126.
    double     avg_price_fix{0.0};
    /// Set true after the first TakeProfit fill so the executor doesn't
    /// re-arm the BE-stop on subsequent partial fills.
    bool       tp1_filled{false};
    std::chrono::system_clock::time_point opened_at;
    double     unrealized_pnl{0.0};

    bool operator==(const ActiveTrade&) const = default;
};

} // namespace trade_bot
