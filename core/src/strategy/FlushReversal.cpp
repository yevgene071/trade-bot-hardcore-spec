#include "FlushReversal.hpp"
#include "logger/Logger.hpp"
#include "numeric/PriceUtils.hpp"

#include <cmath>
#include <algorithm>

namespace trade_bot {

FlushReversal::FlushReversal(Ticker ticker, TickerInfo info, const Config& cfg)
    : ticker_(std::move(ticker))
    , info_(std::move(info))
    , name_("FlushReversal")
    , cfg_(cfg) {}

FlushReversal::FlushReversal(Ticker ticker, TickerInfo info)
    : FlushReversal(std::move(ticker), std::move(info), Config{}) {}

void FlushReversal::on_frame(const FeatureFrame& frame) {
    if (frame.ticker == ticker_) ctx_.update(frame);
}

void FlushReversal::on_signal(const Signal& signal) {
    if (signal.ticker == ticker_) ctx_.update(signal);
}

std::optional<TradePlan> FlushReversal::tick(std::chrono::system_clock::time_point now) {
    std::lock_guard<std::mutex> lock(ctx_.mtx_);

    if (active_plan_) {
        if (now > active_plan_->valid_until) active_plan_ = std::nullopt;
        else return std::nullopt;
    }

    const auto& frame = ctx_.last_frame;
    if (frame.ticker.empty()) return std::nullopt;

    // C1: Spread gate — wide spread means panic / low liquidity, skip
    if (frame.spread_bps > cfg_.max_spread_bps) {
        LOG_TRACE("[Flush] {} spread too wide: {:.1f}", ticker_, frame.spread_bps);
        return std::nullopt;
    }

    // C2: Fresh LevelBreak — the defining event of the pattern
    auto it_break = ctx_.recent_signals.find(SignalKind::LevelBreak);
    if (it_break == ctx_.recent_signals.end() ||
        (now - it_break->second.timestamp) > cfg_.flush_max_age) {
        return std::nullopt;
    }
    const Signal& brk = it_break->second;
    const double level_price = brk.price;

    // dist_bps sign encodes direction: < 0 → price below level (flushed down) → Buy reversal
    //                                  > 0 → price above level (flushed up)   → Sell reversal
    const bool flushed_down = (brk.payload.dist_bps < 0.0);
    const Side plan_side = flushed_down ? Side::Buy : Side::Sell;

    // Break must have pushed meaningfully past the level (not a gentle touch)
    if (std::abs(brk.payload.dist_bps) < cfg_.min_flush_dist_bps) {
        LOG_TRACE("[Flush] {} break too shallow: {:.1f} bps (need {:.1f})",
                  ticker_, std::abs(brk.payload.dist_bps), cfg_.min_flush_dist_bps);
        return std::nullopt;
    }

    // C3: Level strength — only trade well-tested S/R
    if (brk.payload.touches < cfg_.min_level_touches) {
        LOG_TRACE("[Flush] {} level weak: {} touches (need {})",
                  ticker_, brk.payload.touches, cfg_.min_level_touches);
        return std::nullopt;
    }

    // C4: TapeFlush confirming the stop-hunt candle
    auto it_flush = ctx_.recent_signals.find(SignalKind::TapeFlush);
    if (it_flush == ctx_.recent_signals.end() ||
        (now - it_flush->second.timestamp) > cfg_.tape_flush_max_age) {
        LOG_TRACE("[Flush] {} no recent TapeFlush", ticker_);
        return std::nullopt;
    }
    // TapeFlush must not predate the LevelBreak by more than 3 s
    if (it_flush->second.timestamp < brk.timestamp - std::chrono::seconds(3)) {
        LOG_TRACE("[Flush] {} TapeFlush predates LevelBreak", ticker_);
        return std::nullopt;
    }

    // C5: Volume fade — aggression in the flush direction must be dying out
    const double vol_flush_1s = flushed_down ? frame.sell_vol_1s : frame.buy_vol_1s;
    const double vol_flush_5s = flushed_down ? frame.sell_vol_5s : frame.buy_vol_5s;
    const double avg_vol_1s   = (vol_flush_5s > 0.0) ? (vol_flush_5s / 5.0) : 1.0;
    const double vol_fade_ratio = vol_flush_1s / std::max(1.0, avg_vol_1s);
    if (vol_fade_ratio >= cfg_.max_vol_fade_ratio) {
        LOG_TRACE("[Flush] {} volume not faded: ratio={:.2f} (need <{:.2f})",
                  ticker_, vol_fade_ratio, cfg_.max_vol_fade_ratio);
        return std::nullopt;
    }

    // C6: Reversal move confirmed — price already moving back toward the level
    // price_change_1s is % (positive = up). For Buy reversal we need upward move.
    const double reversal_bps = (flushed_down ? frame.price_change_1s : -frame.price_change_1s)
                                * 100.0;
    if (reversal_bps < cfg_.min_price_reversal_bps) {
        LOG_TRACE("[Flush] {} reversal not confirmed: {:.1f} bps (need {:.1f})",
                  ticker_, reversal_bps, cfg_.min_price_reversal_bps);
        return std::nullopt;
    }

    // C7: No TapeBurst in the continuation direction (would indicate real breakout, not flush)
    {
        auto it_burst = ctx_.recent_signals.find(SignalKind::TapeBurst);
        if (it_burst != ctx_.recent_signals.end() &&
            (now - it_burst->second.timestamp) <= std::chrono::seconds(5)) {
            std::string_view burst_side = it_burst->second.payload.side;
            const bool is_continuation = (flushed_down && burst_side == "Sell") ||
                                         (!flushed_down && burst_side == "Buy");
            if (is_continuation) {
                LOG_TRACE("[Flush] {} TapeBurst confirms real breakout — skip", ticker_);
                return std::nullopt;
            }
        }
    }

    // C8: No DensityEating in the flush direction (institutional continuation pressure)
    {
        auto it_eat = ctx_.recent_signals.find(SignalKind::DensityEating);
        if (it_eat != ctx_.recent_signals.end() &&
            (now - it_eat->second.timestamp) <= std::chrono::seconds(5)) {
            std::string_view eat_side = it_eat->second.payload.side;
            const bool is_continuation = (flushed_down && eat_side == "Ask") ||
                                         (!flushed_down && eat_side == "Bid");
            if (is_continuation) {
                LOG_TRACE("[Flush] {} DensityEating confirms real breakout — skip", ticker_);
                return std::nullopt;
            }
        }
    }

    LOG_INFO("[Strategy] FlushReversal TRIGGERED for {}! {} at level {:.4f} (vol_fade={:.2f})",
             ticker_, flushed_down ? "BUY" : "SELL", level_price, vol_fade_ratio);

    // Entry just inside the broken level so we're positioned as price returns through it.
    // Stop beyond the level to invalidate if the real breakout resumes.
    const double offset = level_price * (cfg_.entry_offset_bps / kBpsBase);
    const double buffer = level_price * (cfg_.stop_buffer_bps  / kBpsBase);
    double entry_price, stop_price, tp1_price;

    if (plan_side == Side::Buy) {
        entry_price = level_price - offset;            // just below the (broken) level
        stop_price  = level_price - buffer;            // further below — flush must not resume
        const double risk = entry_price - stop_price;
        tp1_price   = entry_price + cfg_.tp1_r * risk; // above original level
    } else {
        entry_price = level_price + offset;
        stop_price  = level_price + buffer;
        const double risk = stop_price - entry_price;
        tp1_price   = entry_price - cfg_.tp1_r * risk;
    }

    entry_price = round_to_tick(entry_price, info_.price_increment);
    stop_price  = round_to_tick(stop_price,  info_.price_increment);
    tp1_price   = round_to_tick(tp1_price,   info_.price_increment);

    TradePlan plan {
        .ticker          = ticker_,
        .side            = plan_side,
        .entry_type      = OrderType::Limit,
        .entry_price     = entry_price,
        .stop_price      = stop_price,
        .tp1_price       = tp1_price,
        .tp2_price       = std::nullopt,
        .tp1_size_ratio  = cfg_.tp1_size_ratio,
        .size_coin       = 0.0,
        .risk_usd        = 0.0,
        .strategy_name   = "FlushReversal",
        .reason          = FixedString<128>::format("Flush %s at %.4f (fade=%.2f, rev=%.1fbps)",
                               flushed_down ? "Buy" : "Sell", level_price,
                               vol_fade_ratio, reversal_bps),
        .evidence        = {brk, it_flush->second},
        .valid_until     = now + cfg_.entry_timeout,
        .no_progress_timeout_sec = cfg_.no_progress_timeout_sec,
        .post_entry_grace_sec    = cfg_.post_entry_grace_sec,
        .min_follow_through_bps  = cfg_.min_follow_through_bps,
    };

    active_plan_ = plan;
    return plan;
}

void FlushReversal::on_plan_accepted(const TradePlan& plan) {
    if (plan.strategy_name == name_) {
        active_trade_info_ = plan;
        LOG_INFO("[Flush] {}: trade accepted at {:.4f}, monitoring post-entry",
                 ticker_, plan.entry_price);
    }
}

void FlushReversal::reset_active_plan() {
    active_plan_       = std::nullopt;
    active_trade_info_ = std::nullopt;
}

std::optional<FixedString<32>> FlushReversal::check_close_conditions(const FeatureFrame&) {
    if (!active_trade_info_) return std::nullopt;
    const auto& plan = *active_trade_info_;
    const auto now = std::chrono::system_clock::now();

    // A second TapeFlush post-entry means the breakout is accelerating — exit
    {
        auto it_flush = ctx_.recent_signals.find(SignalKind::TapeFlush);
        if (it_flush != ctx_.recent_signals.end() &&
            (now - it_flush->second.timestamp) <= std::chrono::seconds(5)) {
            LOG_WARN("[Flush] {} second TapeFlush post-entry — breakout resuming, closing",
                     ticker_);
            active_trade_info_ = std::nullopt;
            return FixedString<32>("SecondFlushPostEntry");
        }
    }

    // Contra TapeBurst: strong aggression against our reversal direction
    {
        auto it_burst = ctx_.recent_signals.find(SignalKind::TapeBurst);
        if (it_burst != ctx_.recent_signals.end() &&
            (now - it_burst->second.timestamp) <= std::chrono::seconds(5)) {
            std::string_view burst_side = it_burst->second.payload.side;
            const bool is_contra = (plan.side == Side::Buy  && burst_side == "Sell") ||
                                   (plan.side == Side::Sell && burst_side == "Buy");
            if (is_contra) {
                LOG_WARN("[Flush] {} contra TapeBurst post-entry — closing", ticker_);
                active_trade_info_ = std::nullopt;
                return FixedString<32>("BurstContraPostEntry");
            }
        }
    }

    return std::nullopt;
}

StrategyState FlushReversal::get_state() const {
    std::lock_guard<std::mutex> lock(ctx_.mtx_);
    StrategyState state;
    state.ticker        = ticker_;
    state.strategy_name = "FlushReversal";

    if (active_plan_) {
        state.ready_state   = StrategyReadyState::Planning;
        state.readiness_pct = 100.0;
        return state;
    }

    const auto& frame = ctx_.last_frame;
    const auto  now   = std::chrono::system_clock::now();

    if (frame.ticker.empty()) {
        state.ready_state = StrategyReadyState::Cold;
        return state;
    }

    int sig_count = 0;
    for (const auto& sig : ctx_.signal_history) {
        if ((now - sig.timestamp) <= std::chrono::seconds(60)) ++sig_count;
    }
    state.signals_last_60s = sig_count;

    std::vector<StrategyCondition> conds;

    {
        StrategyCondition c;
        c.name    = "Spread";
        c.unit    = "bps";
        c.current = frame.spread_bps;
        c.target  = cfg_.max_spread_bps;
        c.met     = (c.current <= c.target);
        conds.push_back(c);
    }
    {
        StrategyCondition c;
        c.name = "LevelBreak";
        c.unit = "count";
        auto it = ctx_.recent_signals.find(SignalKind::LevelBreak);
        c.met     = (it != ctx_.recent_signals.end() &&
                     (now - it->second.timestamp) <= cfg_.flush_max_age);
        c.current = c.met ? 1.0 : 0.0;
        c.target  = 1.0;
        conds.push_back(c);
    }
    {
        StrategyCondition c;
        c.name = "TapeFlush";
        c.unit = "count";
        auto it = ctx_.recent_signals.find(SignalKind::TapeFlush);
        c.met     = (it != ctx_.recent_signals.end() &&
                     (now - it->second.timestamp) <= cfg_.tape_flush_max_age);
        c.current = c.met ? 1.0 : 0.0;
        c.target  = 1.0;
        conds.push_back(c);
    }
    {
        StrategyCondition c;
        c.name = "Vol fade";
        c.unit = "ratio";
        // Show min fade ratio across both sides (lower = more faded = better)
        double r_sell = 1.0, r_buy = 1.0;
        if (frame.sell_vol_5s > 0) r_sell = frame.sell_vol_1s / (frame.sell_vol_5s / 5.0);
        if (frame.buy_vol_5s  > 0) r_buy  = frame.buy_vol_1s  / (frame.buy_vol_5s  / 5.0);
        c.current = std::min(r_sell, r_buy);
        c.target  = cfg_.max_vol_fade_ratio;
        c.met     = (c.current < c.target);
        conds.push_back(c);
    }
    {
        StrategyCondition c;
        c.name = "Reversal";
        c.unit = "bps";
        c.current = std::abs(frame.price_change_1s) * 100.0;
        c.target  = cfg_.min_price_reversal_bps;
        c.met     = (c.current >= c.target);
        conds.push_back(c);
    }

    state.conditions = std::move(conds);
    int met = 0;
    for (const auto& c : state.conditions) { if (c.met) ++met; }
    state.readiness_pct = state.conditions.empty() ? 0.0
        : static_cast<double>(met) / static_cast<double>(state.conditions.size()) * 100.0;
    state.ready_state = (met == static_cast<int>(state.conditions.size()))
        ? StrategyReadyState::Ready : StrategyReadyState::Warming;

    return state;
}

} // namespace trade_bot
