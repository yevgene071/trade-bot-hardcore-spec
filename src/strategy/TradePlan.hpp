#pragma once

#include "domain/Types.hpp"
#include "signals/Signal.hpp"
#include "utils/FixedString.hpp"

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
    FixedString<32> strategy_name;
    FixedString<128> reason;
    std::vector<Signal> evidence;
    std::chrono::system_clock::time_point valid_until;

    // ── Post-entry invalidation (STRATEGIES.md § 0.4, § 2.7, § 3.7) ──────────
    /// If price doesn't move in our favour within this timeout (seconds),
    /// the executor closes the position by market. Per-strategy default:
    /// Bounce=120s, Breakout=60s, LeaderLag=15s.
    double no_progress_timeout_sec{120.0};
    /// Grace period after entry during which follow-through is not enforced.
    /// Used by BreakoutEatThrough (default 5s).
    double post_entry_grace_sec{5.0};
    /// Minimum price move (in bps) required within post_entry_grace_sec.
    /// Used by BreakoutEatThrough (default 10 bps).
    double min_follow_through_bps{10.0};

    // ── Post-entry invalidation monitoring (STRATEGIES.md § 3.7) ──────────
    /// Entry-time values used by strategies (e.g., LeaderLag) to detect
    /// correlation breakdown and leader reversal after the trade is open.
    double entry_correlation{0.0};
    double leader_entry_lag_pct{0.0};
    double leader_entry_move_1s{0.0};
    /// Minimum leader correlation before trade is force-closed. 0 = disabled.
    double correlation_exit_threshold{0.0};
    /// Leader reversal (bps) beyond which trade is force-closed. 0 = disabled.
    double leader_exit_reversal_bps{0.0};
    /// Price of the density used for stop calculation (BounceFromDensity).
    double density_price_for_stop{0.0};

    bool operator==(const TradePlan&) const = default;
};

} // namespace trade_bot
