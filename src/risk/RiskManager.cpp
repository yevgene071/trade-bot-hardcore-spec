#include "RiskManager.hpp"
#include "logger/Logger.hpp"

#include <cmath>
#include <algorithm>
#include <iostream>

namespace trade_bot {

RiskManager::RiskManager(const TickerUniverse& universe,
                        const NewsCalendar& news,
                        Config cfg)
    : universe_(universe)
    , news_(news)
    , cfg_(cfg) {}

RiskManager::RiskManager(const TickerUniverse& universe,
                        const NewsCalendar& news)
    : RiskManager(universe, news, Config{}) {}

RiskDecision RiskManager::evaluate(const TradePlan& plan, const AccountState& state) {
    RiskDecision d;
    auto now = std::chrono::system_clock::now();

    // R1. Kill-switch
    if (state.kill_switch_triggered) {
        d.reason = RejectReason::KillSwitchActive;
        d.details = "Kill-switch is active";
        return d;
    }

    // R2. Daily loss limit
    double daily_pnl_pct = (state.realized_pnl_today_usd + state.unrealized_pnl_usd) / state.starting_equity_usd * 100.0;
    if (daily_pnl_pct <= -cfg_.max_daily_loss_pct) {
        d.reason = RejectReason::DailyLossLimitHit;
        d.details = "Daily loss limit hit: " + std::to_string(daily_pnl_pct) + "%";
        return d;
    }

    // R3. Concurrent positions
    if (state.active_positions >= cfg_.max_concurrent_positions) {
        d.reason = RejectReason::TooManyPositions;
        d.details = "Too many concurrent positions: " + std::to_string(state.active_positions);
        return d;
    }

    // R4. Unique ticker / Hedge
    bool already_has_ticker = std::find(state.active_tickers.begin(), state.active_tickers.end(), plan.ticker) != state.active_tickers.end();
    if (already_has_ticker && !cfg_.allow_hedge) {
        d.reason = RejectReason::DuplicatePosition;
        d.details = "Already has position in " + plan.ticker;
        return d;
    }

    // R5. Universe
    if (!universe_.is_boosted(plan.ticker, now)) { 
         if (std::find(cfg_.whitelist_tickers.begin(), cfg_.whitelist_tickers.end(), plan.ticker) == cfg_.whitelist_tickers.end()) {
            d.reason = RejectReason::NotInUniverse;
            d.details = "Ticker not in tradable universe or whitelist: " + plan.ticker;
            return d;
         }
    }

    // R6. Stop validation
    double stop_dist_bps = std::abs(plan.entry_price - plan.stop_price) / plan.entry_price * 10000.0;
    bool stop_correct_side = (plan.side == Side::Buy && plan.stop_price < plan.entry_price) ||
                            (plan.side == Side::Sell && plan.stop_price > plan.entry_price);
    
    if (!stop_correct_side) {
        d.reason = RejectReason::InvalidStopSide;
        d.details = "Stop price on wrong side of entry";
        return d;
    }

    if (stop_dist_bps < cfg_.min_stop_bps) {
        d.reason = RejectReason::StopTooTight;
        d.details = "Stop too tight: " + std::to_string(stop_dist_bps) + " bps";
        return d;
    }

    if (stop_dist_bps > cfg_.max_stop_bps) {
        d.reason = RejectReason::StopTooWide;
        d.details = "Stop too wide: " + std::to_string(stop_dist_bps) + " bps";
        return d;
    }

    // R7. TP validation
    double tp1_dist_bps = std::abs(plan.tp1_price - plan.entry_price) / plan.entry_price * 10000.0;
    if (tp1_dist_bps < cfg_.min_rr_ratio * stop_dist_bps) {
        d.reason = RejectReason::PoorRewardRisk;
        d.details = "TP1 R:R too low";
        return d;
    }

    // R8. Sizing
    double risk_usd_target = state.equity_usd * cfg_.max_per_trade_risk_pct / 100.0;
    double size_coin = risk_usd_target / (plan.entry_price * stop_dist_bps / 10000.0);

    double max_val_usd = state.equity_usd * cfg_.max_position_value_pct / 100.0;
    if (size_coin * plan.entry_price > max_val_usd) {
        size_coin = max_val_usd / plan.entry_price;
    }

    d.adjusted_size_coin = size_coin;
    d.risk_usd = size_coin * plan.entry_price * stop_dist_bps / 10000.0;

    // R9. Margin
    double required_margin = (size_coin * plan.entry_price) / cfg_.max_leverage;
    if (required_margin > state.free_balance_usd * cfg_.margin_safety_ratio) {
        d.reason = RejectReason::InsufficientMargin;
        d.details = "Insufficient margin";
        return d;
    }

    // R10. Rate limit
    while (!trade_history_.empty() && (now - trade_history_.front()) > std::chrono::minutes(cfg_.trades_window_min)) {
        trade_history_.pop_front();
    }
    if (trade_history_.size() >= static_cast<size_t>(cfg_.max_trades_per_window)) {
        d.reason = RejectReason::TradeRateLimitHit;
        d.details = "Trade rate limit hit";
        return d;
    }

    // R11. Loss streak
    if (last_loss_streak_ts_ != std::chrono::system_clock::time_point{} &&
        (now - last_loss_streak_ts_) < std::chrono::minutes(cfg_.loss_streak_cooloff_min)) {
        d.reason = RejectReason::LossStreakCircuitBreaker;
        d.details = "Loss streak cooloff active";
        return d;
    }

    // R12. News blackout
    auto mins = news_.minutes_to_next_news(now, plan.ticker);
    if (mins && std::abs(*mins) <= cfg_.news_blackout_min) {
        d.reason = RejectReason::NewsBlackout;
        d.details = "News blackout active";
        return d;
    }

    // R13. Funding blackout
    auto fit = funding_times_.find(plan.ticker);
    if (fit != funding_times_.end()) {
        auto delta = now - fit->second;
        auto secs = std::abs(std::chrono::duration_cast<std::chrono::seconds>(delta).count());
        if (secs <= cfg_.funding_blackout_pre_sec || secs <= cfg_.funding_blackout_post_sec) {
            d.reason = RejectReason::FundingBlackout;
            d.details = "Funding blackout active";
            return d;
        }
    }

    d.accepted = true;
    return d;
}

void RiskManager::update_funding_time(const Ticker& ticker, std::chrono::system_clock::time_point ts) {
    funding_times_[ticker] = ts;
}

void RiskManager::record_trade_end(bool is_loss, std::chrono::system_clock::time_point ts) {
    trade_history_.push_back(ts);
    loss_history_.push_back({ts, is_loss});
    
    while (!loss_history_.empty() && (ts - loss_history_.front().first) > std::chrono::minutes(cfg_.loss_streak_window_min)) {
        loss_history_.pop_front();
    }
    
    int consecutive_losses = 0;
    for (auto it = loss_history_.rbegin(); it != loss_history_.rend(); ++it) {
        if (it->second) consecutive_losses++;
        else break;
    }
    
    if (consecutive_losses >= cfg_.max_consecutive_losses) {
        last_loss_streak_ts_ = ts;
        // LOG_WARN("RiskManager: loss streak circuit breaker triggered");
    }
}

} // namespace trade_bot
