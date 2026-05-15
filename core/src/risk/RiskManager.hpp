#pragma once

#include "AccountState.hpp"
#include "strategy/TradePlan.hpp"
#include "risk/NewsCalendar.hpp"
#include "universe/TickerUniverse.hpp"

#include <deque>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace trade_bot {

/**
 * T4-RISK: Final gatekeeper for all TradePlans.
 * Enforces rules R1-R13.
 */
class RiskManager {
public:
    struct Config {
        double max_daily_loss_pct{3.0};
        int    max_concurrent_positions{3};
        int    max_positions_per_ticker{1};
        bool   allow_hedge{false};
        
        double min_stop_bps{3.0};
        double max_stop_bps{20.0};
        double warn_stop_bps{15.0};
        double min_rr_ratio{1.0};
        double min_tp2_rr{2.0};
        
        double max_per_trade_risk_pct{0.5};
        double max_position_value_pct{10.0};
        
        double margin_safety_ratio{0.8};
        int    max_leverage{5};
        
        int    trades_window_min{5};
        int    max_trades_per_window{6};
        
        int    loss_streak_window_min{15};
        int    max_consecutive_losses{3};
        int    loss_streak_cooloff_min{10};
        
        int    news_blackout_min{5};
        int    news_calendar_check_min{60};
        bool   news_calendar_require_fresh{false};
        int    funding_blackout_pre_sec{30};
        int    funding_blackout_post_sec{30};
        
        std::vector<std::string> whitelist_tickers;
    };

    RiskManager(const TickerUniverse& universe, 
                const NewsCalendar& news,
                const Config& cfg);

    explicit RiskManager(const TickerUniverse& universe, 
                         const NewsCalendar& news);

    /// Evaluate a TradePlan against R1..R13. Thread-safe — multiple
    /// threads (strategy engine + executor reconciliation) may call this
    /// concurrently. Mutates anti-tilt state (trade_history_).
    RiskDecision evaluate(const TradePlan& plan, const AccountState& state);

    void record_trade_end(bool is_loss, std::chrono::system_clock::time_point ts);

    /// Push the **next** funding-event timestamp for `ticker`. Used by R13
    /// to reject inside `[next - pre_sec, next + post_sec]`. The previous
    /// semantics (last funding time only) blocked post-funding correctly
    /// but never pre-funding, defeating the whole point of the window
    /// (#127). Callers should populate this from
    /// `BinanceFundingClient::next_funding_time_utc`.
    void update_funding_time(const Ticker& ticker, std::chrono::system_clock::time_point ts);

private:
    const TickerUniverse& universe_;
    const NewsCalendar&   news_;
    Config                cfg_;

    // All mutable bookkeeping below is guarded by mtx_. Without it,
    // evaluate() and record_trade_end() racing on trade_history_ /
    // loss_history_ is UB (deque push/pop is not atomic). Issue #125.
    mutable std::mutex                                              mtx_;
    std::deque<std::chrono::system_clock::time_point>               trade_history_;
    std::deque<std::pair<std::chrono::system_clock::time_point, bool>> loss_history_;
    std::chrono::system_clock::time_point                           last_loss_streak_ts_;
    std::unordered_map<Ticker, std::chrono::system_clock::time_point> funding_times_;
    // T1-BUGFIX: Track previous funding timestamp for post-funding blackout (#204).
    // funding_times_ holds the NEXT event; prev_funding_times_ holds the most recent
    // PAST event so the post-window survives the immediate WS update of next_funding_time.
    std::unordered_map<Ticker, std::chrono::system_clock::time_point> prev_funding_times_;
};

} // namespace trade_bot
