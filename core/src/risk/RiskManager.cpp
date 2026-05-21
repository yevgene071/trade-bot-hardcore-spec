#include "RiskManager.hpp"
#include "logger/Logger.hpp"

#include <algorithm>
#include <cmath>

namespace trade_bot {

RiskManager::RiskManager(const TickerUniverse& universe,
                        const NewsCalendar& news,
                        const Config& cfg,
                        std::shared_ptr<IClock> clock)
    : universe_(universe)
    , news_(news)
    , cfg_(cfg)
    , clock_(std::move(clock)) {}

RiskManager::RiskManager(const TickerUniverse& universe,
                        const NewsCalendar& news,
                        std::shared_ptr<IClock> clock)
    : RiskManager(universe, news, Config{}, std::move(clock)) {}

RiskDecision RiskManager::evaluate(const TradePlan& plan, const AccountState& state) {
    RiskDecision d;
    // P0-DETERMINISM: Use injected clock for replay determinism
    auto now = clock_ ? clock_->now() : std::chrono::system_clock::now();

    // ---- Input sanity (#125): reject malformed inputs explicitly so we
    //      never divide by zero further down. Each guard short-circuits
    //      to RejectReason::InternalError + LOG_ERROR so the operator
    //      sees a clear cause instead of silent NaNs.
    if (state.starting_equity_usd <= 0.0) {
        LOG_ERROR("RiskManager: starting_equity_usd={} is not positive — rejecting plan",
                  state.starting_equity_usd);
        d.reason  = RejectReason::InternalError;
        d.details = "starting_equity_usd is not positive";
        return d;
    }
    if (plan.entry_price <= 0.0) {
        LOG_ERROR("RiskManager: plan.entry_price={} for {} is not positive",
                  plan.entry_price, plan.ticker);
        d.reason  = RejectReason::InternalError;
        d.details = "entry_price is not positive";
        return d;
    }
    if (cfg_.max_leverage <= 0) {
        LOG_ERROR("RiskManager: cfg.max_leverage={} is not positive — bad config",
                  cfg_.max_leverage);
        d.reason  = RejectReason::InternalError;
        d.details = "max_leverage is not positive";
        return d;
    }

    // R1. Kill-switch
    if (state.kill_switch_triggered) {
        d.reason = RejectReason::KillSwitchActive;
        d.details = "Kill-switch is active";
        return d;
    }

    // R2. Daily loss limit (starting_equity_usd guarded above).
    // Per spec §8: daily P&L = realized_today + unrealized (mark-to-market).
    // Unrealized is included so an open drawdown triggers the limit before close.
    double daily_pnl_pct =
        (state.realized_pnl_today_usd + state.unrealized_pnl_usd) /
        state.starting_equity_usd * 100.0;
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

    // R4b: Per-ticker position limit
    int ticker_count = static_cast<int>(std::count(state.active_tickers.begin(), state.active_tickers.end(), plan.ticker));
    if (ticker_count >= cfg_.max_positions_per_ticker) {
        d.reason = RejectReason::DuplicatePosition;
        d.details = "Per-ticker limit reached for " + plan.ticker + ": " + std::to_string(ticker_count) + "/" + std::to_string(cfg_.max_positions_per_ticker);
        return d;
    }

    // R5. Universe — accept if ticker is in the active pool, boosted, or explicitly whitelisted.
    // Additionally check strategy-specific affinity (ARCHITECTURE.md § 2.11).
    {
        const bool in_pool = universe_.is_in_pool(plan.ticker);
        const bool boosted  = universe_.is_boosted(plan.ticker, now);
        const bool listed   = !cfg_.whitelist_tickers.empty() &&
            std::find(cfg_.whitelist_tickers.begin(), cfg_.whitelist_tickers.end(), plan.ticker)
                != cfg_.whitelist_tickers.end();
        if (!in_pool && !boosted && !listed) {
            d.reason  = RejectReason::NotInUniverse;
            d.details = "Ticker not in tradable universe or whitelist: " + plan.ticker;
            return d;
        }

        // Strategy-specific affinity: ticker must be enabled for this strategy.
        // Per ARCHITECTURE.md § 2.11 — TickerUniverse::is_tradable_by().
        if (!boosted && !listed && !universe_.is_strategy_enabled(plan.ticker, std::string(plan.strategy_name))) {
            d.reason  = RejectReason::NotInUniverse;
            d.details = "Ticker " + plan.ticker + " not enabled for strategy " + std::string(plan.strategy_name);
            return d;
        }
    }

    // R6. Stop validation
    if (plan.stop_price <= 0.0) {
        d.reason = RejectReason::InvalidStopSide;
        d.details = "Stop price must be positive";
        return d;
    }
    double stop_dist_bps = std::abs(plan.entry_price - plan.stop_price) / plan.entry_price * 10000.0;
    if (stop_dist_bps <= 0.0) {
        d.reason = RejectReason::StopTooTight;
        d.details = "Stop distance is zero";
        return d;
    }

    bool stop_correct_side = (plan.side == Side::Buy && plan.stop_price < plan.entry_price) ||
                            (plan.side == Side::Sell && plan.stop_price > plan.entry_price);
    
    if (!stop_correct_side) {
        d.reason = RejectReason::InvalidStopSide;
        d.details = "Stop price on wrong side of entry";
        return d;
    }

    // Per-ticker stop bounds: small alts have lower volume → wider stops are expected.
    // inv_sqrt ∈ [0.45, 2.58]; bounds scale proportionally up for small alts.
    const double sf_stop    = universe_.volume_scale_factor(plan.ticker);
    const double inv_sqrt_s = 1.0 / std::sqrt(sf_stop);
    const double max_stop   = std::min(cfg_.max_stop_bps  * inv_sqrt_s, 100.0);
    const double warn_stop  = std::min(cfg_.warn_stop_bps * inv_sqrt_s, 80.0);

    if (stop_dist_bps < cfg_.min_stop_bps) {
        d.reason = RejectReason::StopTooTight;
        d.details = "Stop too tight: " + std::to_string(stop_dist_bps) + " bps";
        return d;
    }

    if (stop_dist_bps > max_stop) {
        d.reason = RejectReason::StopTooWide;
        d.details = "Stop too wide: " + std::to_string(stop_dist_bps) + " bps (max=" + std::to_string(max_stop) + ")";
        return d;
    }

    if (stop_dist_bps > warn_stop) {
        LOG_WARN("RiskManager: stop distance {:.1f} bps > warn threshold {:.1f} bps for {}",
                 stop_dist_bps, warn_stop, plan.ticker);
    }

    // R7. TP validation
    if (plan.tp1_price <= 0.0) {
        d.reason = RejectReason::PoorRewardRisk;
        d.details = "TP1 price must be positive";
        return d;
    }

    bool tp_correct_side = (plan.side == Side::Buy && plan.tp1_price > plan.entry_price) ||
                           (plan.side == Side::Sell && plan.tp1_price < plan.entry_price);
    
    if (!tp_correct_side) {
        d.reason = RejectReason::PoorRewardRisk;
        d.details = "TP1 price on wrong side of entry";
        return d;
    }

    double tp1_dist_bps = std::abs(plan.tp1_price - plan.entry_price) / plan.entry_price * 10000.0;
    if (tp1_dist_bps < cfg_.min_rr_ratio * stop_dist_bps) {
        d.reason = RejectReason::PoorRewardRisk;
        d.details = "TP1 R:R too low";
        return d;
    }

    if (plan.tp2_price && *plan.tp2_price > 0.0) {
        bool tp2_correct_side = (plan.side == Side::Buy && *plan.tp2_price > plan.entry_price) ||
                                (plan.side == Side::Sell && *plan.tp2_price < plan.entry_price);
        if (!tp2_correct_side) {
            d.reason = RejectReason::PoorRewardRisk;
            d.details = "TP2 price on wrong side of entry";
            return d;
        }
    }

    // R8. Sizing — stop_dist_bps validated in R6 (B2-FIX), guaranteed > 0
    double risk_usd_target = state.equity_usd * cfg_.max_per_trade_risk_pct / 100.0;
    double size_coin = risk_usd_target / (plan.entry_price * stop_dist_bps / 10000.0);

    // T4-RISK: Price and Size Normalization (#131) — moved position value cap AFTER normalization
    // Round size down first, then compute risk_usd from the REAL size.
    auto meta = universe_.meta(plan.ticker).value_or(TickerMeta{0.01, 1e-6, 0.0, 0.0});
    if (meta.price_increment > 0.0 && meta.size_increment > 0.0) {
        int64_t size_ticks = static_cast<int64_t>(std::floor(size_coin / meta.size_increment + 1e-9));
        d.adjusted_size_coin = static_cast<double>(size_ticks) * meta.size_increment;

        // Verify min size
        if (d.adjusted_size_coin < meta.min_size) {
            d.reason = RejectReason::SizeBelowMinimum;
            d.details = "Size " + std::to_string(d.adjusted_size_coin) + " < min " + std::to_string(meta.min_size);
            return d;
        }
    } else {
        d.adjusted_size_coin = size_coin;
    }

    // Compute risk_usd AFTER size normalization so risk reflects actual sized order.
    // Previously risk_usd was computed from the raw size_coin before rounding down,
    // which could overstate risk for small-sized trades.
    d.risk_usd = d.adjusted_size_coin * plan.entry_price * stop_dist_bps / 10000.0;

    // Apply position value cap AFTER risk_usd is computed from the adjusted size.
    double max_val_usd = state.equity_usd * cfg_.max_position_value_pct / 100.0;
    if (d.adjusted_size_coin * plan.entry_price > max_val_usd) {
        double capped_size = max_val_usd / plan.entry_price;
        if (meta.size_increment > 0.0) {
            int64_t capped_ticks = static_cast<int64_t>(std::floor(capped_size / meta.size_increment + 1e-9));
            capped_size = static_cast<double>(capped_ticks) * meta.size_increment;
        }
        d.adjusted_size_coin = capped_size;
        d.risk_usd = d.adjusted_size_coin * plan.entry_price * stop_dist_bps / 10000.0;
    }

    // R9. Margin
    double required_margin = (d.adjusted_size_coin * plan.entry_price) / cfg_.max_leverage;
    if (required_margin > state.free_balance_usd * cfg_.margin_safety_ratio) {
        d.reason = RejectReason::InsufficientMargin;
        d.details = "Insufficient margin";
        return d;
    }

    // R10–R13 access shared mutable state; lock only here, not over R1–R9.
    std::lock_guard lock(mtx_);

    // R10. Rate limit
    while (trade_history_.size() > cfg_.max_trade_history) trade_history_.pop_front();
    
    while (!trade_history_.empty() && (now - trade_history_.front()) > std::chrono::minutes(cfg_.trades_window_min)) {
        trade_history_.pop_front();
    }
    if (trade_history_.size() >= static_cast<size_t>(cfg_.max_trades_per_window)) {
        d.reason = RejectReason::TradeRateLimitHit;
        d.details = "Trade rate limit hit";
        return d;
    }

    // R11. Loss streak
    if (last_loss_streak_ts_ != std::chrono::system_clock::time_point{}) {
        if ((now - last_loss_streak_ts_) < std::chrono::minutes(cfg_.loss_streak_cooloff_min)) {
            d.reason = RejectReason::LossStreakCircuitBreaker;
            d.details = "Loss streak cooloff active";
            return d;
        }
        // Cooloff expired: clear stale history so the very next loss doesn't re-arm immediately.
        loss_history_.clear();
        last_loss_streak_ts_ = {};
    }

    // R12. News blackout
    auto mins = news_.minutes_to_next_news(now, plan.ticker);
    if (mins && std::abs(*mins) <= cfg_.news_blackout_min) {
        d.reason = RejectReason::NewsBlackout;
        d.details = "News blackout active";
        return d;
    }

    // R12b. News calendar freshness check
    // B10-FIX: Check both past and future events to detect stale calendar
    auto mins_since_latest = news_.minutes_since_latest_event(now);
    auto mins_to_next = news_.minutes_to_next_news(now, "");  // global check
    
    bool is_stale = false;
    const int64_t stale_threshold = static_cast<int64_t>(cfg_.news_calendar_check_min);
    
    // Calendar is stale if latest event is too old (> threshold hours in past)
    if (mins_since_latest && *mins_since_latest > stale_threshold * 60) {
        is_stale = true;
    }
    // OR if next event is too far (> threshold hours in future)
    if (mins_to_next && *mins_to_next > stale_threshold * 60) {
        is_stale = true;
    }
    
    if (is_stale) {
        LOG_WARN("RiskManager: news calendar stale (latest={} min ago, next={} min)",
                 mins_since_latest.value_or(-1), mins_to_next.value_or(-1));
        if (cfg_.news_calendar_require_fresh) {
            d.reason = RejectReason::NewsBlackout;
            d.details = "News calendar is stale";
            return d;
        }
    }

    // R13. Funding blackout — pre-window (before next funding)
    auto fit = funding_times_.find(plan.ticker);
    if (fit != funding_times_.end()) {
        const auto next_funding = fit->second;
        const auto secs_to_next =
            std::chrono::duration_cast<std::chrono::seconds>(next_funding - now).count();
        if (secs_to_next > 0 && secs_to_next <= cfg_.funding_blackout_pre_sec) {
            d.reason  = RejectReason::FundingBlackout;
            d.details = "Funding blackout (pre-window)";
            return d;
        }
    }
    // T1-BUGFIX: Post-funding blackout checked against the PREVIOUS funding
    // timestamp (#204). Previously in_post used funding_times_ (the NEXT event),
    // which gets overwritten immediately after funding by the WS update,
    // making the post-window effectively non-existent.
    auto pit = prev_funding_times_.find(plan.ticker);
    if (pit != prev_funding_times_.end()) {
        const auto secs_since =
            std::chrono::duration_cast<std::chrono::seconds>(now - pit->second).count();
        if (secs_since >= 0 && secs_since <= cfg_.funding_blackout_post_sec) {
            d.reason  = RejectReason::FundingBlackout;
            d.details = "Funding blackout (post-window)";
            return d;
        }
    }

    // R15. Entry slippage check
    if (state.mark_price > 0.0) {
        double slippage_bps = std::abs(state.mark_price - plan.entry_price) / state.mark_price * 10000.0;
        if (slippage_bps > cfg_.max_entry_slippage_bps) {
            d.reason = RejectReason::EntrySlippageExceeded;
            d.details = "Entry slippage too high: " + std::to_string(slippage_bps) + " bps";
            return d;
        }
    }

    d.accepted = true;
    // P0-DETERMINISM: Use injected clock for replay determinism
    trade_history_.push_back(clock_ ? clock_->now() : std::chrono::system_clock::now());
    return d;
}

void RiskManager::update_funding_time(const Ticker& ticker,
                                      std::chrono::system_clock::time_point ts) {
    std::lock_guard lock(mtx_);
    // B3-FIX: Always save previous timestamp when updating, and handle first update
    auto it = funding_times_.find(ticker);
    if (it != funding_times_.end()) {
        prev_funding_times_[ticker] = it->second;
    } else {
        auto now = clock_ ? clock_->now() : std::chrono::system_clock::now();
        if (ts < now) {
            prev_funding_times_[ticker] = ts;
        }
    }
    funding_times_[ticker] = ts;
}

void RiskManager::record_trade_end(bool is_loss,
                                   std::chrono::system_clock::time_point ts) {
    std::lock_guard lock(mtx_);
    // P0-DETERMINISM: Use provided timestamp (from clock injection upstream)
    loss_history_.push_back({ts, is_loss});

    while (loss_history_.size() > cfg_.max_loss_history) loss_history_.pop_front();
    
    while (!loss_history_.empty() &&
           (ts - loss_history_.front().first) >
               std::chrono::minutes(cfg_.loss_streak_window_min)) {
        loss_history_.pop_front();
    }

    auto first_non_loss = std::find_if(loss_history_.rbegin(), loss_history_.rend(), [](const auto& p) {
        return !p.second;
    });
    int consecutive_losses = static_cast<int>(std::distance(loss_history_.rbegin(), first_non_loss));

    if (consecutive_losses >= cfg_.max_consecutive_losses) {
        last_loss_streak_ts_ = ts;
        LOG_WARN("RiskManager: loss-streak circuit breaker triggered "
                 "({} consecutive losses; cooloff for {} min)",
                 consecutive_losses, cfg_.loss_streak_cooloff_min);
    }
}

} // namespace trade_bot
