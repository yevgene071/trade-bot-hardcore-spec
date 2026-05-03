#pragma once

#include "domain/Types.hpp"
#include <chrono>
#include <vector>
#include <string>

namespace trade_bot {

/**
 * T4-RISK: Current account state for risk evaluation.
 */
struct AccountState {
    double equity_usd{0.0};            // current capital (free + value of open positions)
    double starting_equity_usd{0.0};   // equity at the start of the trading day (UTC 00:00)
    double realized_pnl_today_usd{0.0}; // finres.Result - finres_day_start_result_usd
    double finres_day_start_result_usd{0.0}; // baseline Result from finres_update at UTC reset
    double unrealized_pnl_usd{0.0};
    double free_balance_usd{0.0};
    
    int    active_positions{0};
    std::vector<Ticker> active_tickers;
    std::chrono::system_clock::time_point trading_day_start;
    bool   kill_switch_triggered{false};
};

enum class RejectReason {
    None,
    KillSwitchActive,
    DailyLossLimitHit,
    TooManyPositions,
    DuplicatePosition,
    NotInUniverse,
    StopTooTight,
    StopTooWide,
    InvalidStopSide,
    PoorRewardRisk,
    SizeBelowMinimum,
    InsufficientMargin,
    TradeRateLimitHit,
    LossStreakCircuitBreaker,
    NewsBlackout,
    FundingBlackout,
    SinglePositionLossExceeded,
    EntrySlippageExceeded,
    InternalError                   // malformed inputs (zero divisor, etc.)
};

struct RiskDecision {
    bool accepted{false};
    RejectReason reason{RejectReason::None};
    double adjusted_size_coin{0.0};
    double risk_usd{0.0};
    std::string details;
};

} // namespace trade_bot
