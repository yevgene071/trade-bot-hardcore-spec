#include "LeaderLag.hpp"
#include "logger/Logger.hpp"
#include "numeric/PriceUtils.hpp"
#include <cmath>
#include <algorithm>
#include <limits>

namespace trade_bot {

LeaderLag::LeaderLag(Ticker ticker, TickerInfo info, const Config& cfg)
    : ticker_(std::move(ticker))
    , info_(std::move(info))
    , name_("LeaderLag")
    , cfg_(cfg) {}

LeaderLag::LeaderLag(Ticker ticker, TickerInfo info)
    : LeaderLag(std::move(ticker), std::move(info), Config{}) {}

void LeaderLag::on_frame(const FeatureFrame& frame) {
    if (frame.ticker == ticker_) {
        ctx_.update(frame);
        // Record mid price into ring buffer for swing-low/high stop placement
        if (frame.mid > 0.0) {
            price_history_[price_history_idx_] = frame.mid;
            price_history_idx_ = (price_history_idx_ + 1) % kPriceHistorySize;
            if (price_history_count_ < kPriceHistorySize) ++price_history_count_;
        }
    }
}

void LeaderLag::on_signal(const Signal& signal) {
    if (signal.ticker == ticker_) {
        ctx_.update(signal);
    }
}

double LeaderLag::find_swing_low(size_t lookback) const {
    if (price_history_count_ < 3) return 0.0;
    size_t count = std::min(lookback, price_history_count_);
    double min_price = std::numeric_limits<double>::max();
    for (size_t i = 0; i < count; ++i) {
        // Walk backwards from most recent
        size_t idx = (price_history_idx_ + kPriceHistorySize - 1 - i) % kPriceHistorySize;
        if (price_history_[idx] > 0.0) {
            min_price = std::min(min_price, price_history_[idx]);
        }
    }
    return (min_price < std::numeric_limits<double>::max()) ? min_price : 0.0;
}

double LeaderLag::find_swing_high(size_t lookback) const {
    if (price_history_count_ < 3) return 0.0;
    size_t count = std::min(lookback, price_history_count_);
    double max_price = 0.0;
    for (size_t i = 0; i < count; ++i) {
        size_t idx = (price_history_idx_ + kPriceHistorySize - 1 - i) % kPriceHistorySize;
        max_price = std::max(max_price, price_history_[idx]);
    }
    return max_price;
}

void LeaderLag::on_plan_accepted(const TradePlan& plan) {
    if (plan.strategy_name == name_) {
        active_trade_info_ = plan;
        LOG_INFO("[LeaderLag] {}: trade accepted, beginning post-entry monitoring (corr={:.2f}, lag={:.4f}%)",
                 ticker_, plan.entry_correlation, plan.leader_entry_lag_pct * 100.0);
    }
}

void LeaderLag::reset_active_plan() {
    active_plan_ = std::nullopt;
    active_trade_info_ = std::nullopt;
    LOG_INFO("[LeaderLag] {}: plan rejected/reset, post-entry monitoring cleared", ticker_);
}

std::optional<FixedString<32>> LeaderLag::check_close_conditions(const FeatureFrame& frame) {
    if (!active_trade_info_) return std::nullopt;

    const auto& plan = *active_trade_info_;

    // Check 1: Correlation breakdown (STRATEGIES.md § 3.7)
    if (plan.correlation_exit_threshold > 0.0 &&
        frame.leader_correlation < plan.correlation_exit_threshold) {
        LOG_WARN("[LeaderLag] {} correlation {:.2f} < threshold {:.2f} — requesting close",
                 ticker_, frame.leader_correlation, plan.correlation_exit_threshold);
        active_trade_info_ = std::nullopt;
        return FixedString<32>("CorrelationBreakdown");
    }

    // Check 2: Leader reversal against our position (STRATEGIES.md § 3.7)
    if (plan.leader_exit_reversal_bps > 0.0) {
        // leader_change_1s > 0 means leader moved up; < 0 means leader moved down
        // For long (Buy): leader moving down is reversal. leader_change_1s < 0
        // For short (Sell): leader moving up is reversal. leader_change_1s > 0
        double leader_reversal = (plan.side == Side::Buy)
            ? -frame.leader_change_1s   // positive = leader moving up (good), negative = reversal
            : frame.leader_change_1s;    // positive = leader moving down (good for short)

        double reversal_bps = leader_reversal * kBpsBase;
        if (leader_reversal > 0 && reversal_bps > plan.leader_exit_reversal_bps) {
            LOG_WARN("[LeaderLag] {} leader reversal {:.1f} bps > {:.1f} bps — requesting close",
                     ticker_, reversal_bps, plan.leader_exit_reversal_bps);
            active_trade_info_ = std::nullopt;
            return FixedString<32>("LeaderReversal");
        }
    }

    return std::nullopt;
}

std::optional<TradePlan> LeaderLag::tick(std::chrono::system_clock::time_point now) {
    std::lock_guard<std::mutex> lock(ctx_.mtx_);
    if (active_plan_) {
        if (now > active_plan_->valid_until) {
            active_plan_ = std::nullopt;
            active_trade_info_ = std::nullopt;
        }
        else return std::nullopt;
    }

    // 1. Check conditions C1-C6 (STRATEGIES.md § 3.4)

    // C1: LeaderMove
    // C2: signal age <= lag_max_age (3000 ms by default)
    auto it_move = ctx_.recent_signals.find(SignalKind::LeaderMove);
    if (it_move == ctx_.recent_signals.end() ||
        (now - it_move->second.timestamp) > cfg_.lag_max_age) return std::nullopt;

    double lag_pct = it_move->second.payload.lag_pct;
    double corr = it_move->second.payload.correlation;
    Side plan_side = (lag_pct > 0) ? Side::Buy : Side::Sell;

    // C3: Rolling correlation
    if (std::abs(corr) < cfg_.min_correlation) return std::nullopt;

    // C4: Our movement has not started yet (|our_change_2s| <= 0.1%).
    // FeatureFrame stores price_change_1s/5s/30s as fractional (e.g. 0.001 = 0.1%).
    // No native 2s window — interpolate between 1s and 5s as the closest proxy:
    // our_2s ≈ price_change_1s + (price_change_5s - price_change_1s) * (1/4).
    const auto& frame = ctx_.last_frame;
    double our_change_2s = frame.price_change_1s +
                           (frame.price_change_5s - frame.price_change_1s) * 0.25;
    if (std::abs(our_change_2s) * 100.0 > cfg_.max_our_change_2s_pct) {
        LOG_TRACE("[LeaderLag] {} rejected: our_change_2s={:.4f}% exceeds {}%",
                  ticker_, our_change_2s * 100.0, cfg_.max_our_change_2s_pct);
        return std::nullopt;
    }

    // C5: No large density on the path within density_on_path_search_bps.
    // Direction-aware: for long, scan asks above mid; for short, scan bids below.
    {
        const double mid_now = frame.mid;
        bool blocked = false;
        for (auto rit = ctx_.signal_history.rbegin();
             rit != ctx_.signal_history.rend(); ++rit) {
            if (rit->kind != SignalKind::DensityDetected) continue;
            if ((now - rit->timestamp) > std::chrono::seconds(15)) break;
            const double dist_bps = (rit->price - mid_now) / mid_now * 10000.0;
            std::string_view side_str = rit->payload.side;
            if (plan_side == Side::Buy) {
                // path = above mid; density on Ask within range = blocker
                if (side_str == "Ask" && dist_bps > 0.0 &&
                    dist_bps <= cfg_.density_on_path_search_bps) {
                    blocked = true;
                    break;
                }
            } else {
                if (side_str == "Bid" && dist_bps < 0.0 &&
                    -dist_bps <= cfg_.density_on_path_search_bps) {
                    blocked = true;
                    break;
                }
            }
        }
        if (blocked) {
            LOG_TRACE("[LeaderLag] {} rejected: large density on path", ticker_);
            return std::nullopt;
        }
    }

    // C6: Spread
    double spread_bps = (frame.best_ask - frame.best_bid) / frame.best_bid * 10000.0;
    if (spread_bps > cfg_.max_spread_bps) return std::nullopt;

    // Calculate Prices
    double mid = frame.mid;
    double entry_price = round_to_tick(mid, info_.price_increment);
    double stop_dist = mid * (cfg_.stop_distance_bps / 10000.0);

    // FIX: stop_price via local extremum (STRATEGIES.md § 3.4)
    // Instead of always mid ± stop_dist, find the nearest swing low/high within
    // the lookback window. This gives a tighter, more relevant stop that respects
    // actual market structure.
    const size_t swing_look = static_cast<size_t>(cfg_.swing_lookback_ticks);
    double stop_price;
    if (plan_side == Side::Buy) {
        double swing_low = find_swing_low(swing_look);
        double default_stop = mid - stop_dist;
        double max_stop = mid - stop_dist * 1.5;  // never further than 1.5x default
        if (swing_low > 0.0 && swing_low > max_stop && swing_low < mid) {
            // Use swing low if it's tighter than default and within bounds
            stop_price = std::max(swing_low, max_stop);
        } else {
            stop_price = default_stop;
        }
    } else {
        double swing_high = find_swing_high(swing_look);
        double default_stop = mid + stop_dist;
        double max_stop = mid + stop_dist * 1.5;
        if (swing_high > 0.0 && swing_high > mid && swing_high < max_stop) {
            stop_price = std::min(swing_high, max_stop);
        } else {
            stop_price = default_stop;
        }
    }
    stop_price = round_to_tick(stop_price, info_.price_increment);

    double expected_catchup_bps = std::abs(lag_pct) * 100.0;
    double tp1_offset = mid * (expected_catchup_bps * cfg_.tp1_catchup_ratio / 10000.0);
    double tp1_price = round_to_tick((plan_side == Side::Buy) ? mid + tp1_offset : mid - tp1_offset, info_.price_increment);

    TradePlan plan {
        .ticker = ticker_,
        .side = plan_side,
        .entry_type = OrderType::Market,
        .entry_price = entry_price,
        .stop_price = stop_price,
        .tp1_price = tp1_price,
        .tp2_price = std::nullopt,
        .tp1_size_ratio = cfg_.tp1_size_ratio,
        .size_coin = 0.0,
        .risk_usd = 0.0,
        .strategy_name = "LeaderLag",
        .reason = "Leader lag detected",
        .evidence = {it_move->second},
        .valid_until = now + cfg_.entry_timeout,
        .no_progress_timeout_sec = 15.0,
        .entry_correlation = corr,
        .leader_entry_lag_pct = lag_pct,
        .leader_entry_move_1s = frame.leader_change_1s,
        .correlation_exit_threshold = cfg_.correlation_exit_threshold,
        .leader_exit_reversal_bps = cfg_.leader_exit_reversal_bps
    };

    active_plan_ = plan;
    return plan;
}

StrategyState LeaderLag::get_state() const {
    std::lock_guard<std::mutex> lock(ctx_.mtx_);
    StrategyState state;
    state.ticker       = ticker_;
    state.strategy_name = "LeaderLag";

    // Determine ready state
    if (active_plan_) {
        state.ready_state = StrategyReadyState::Planning;
        state.readiness_pct = 100.0;
        return state;
    }

    const auto& frame = ctx_.last_frame;
    const auto now = std::chrono::system_clock::now();

    if (frame.ticker.empty()) {
        state.ready_state = StrategyReadyState::Cold;
        state.readiness_pct = 0.0;
        return state;
    }

    // Count signals last 60s
    int sig_count = 0;
    for (const auto& sig : ctx_.signal_history) {
        if ((now - sig.timestamp) <= std::chrono::seconds(60)) ++sig_count;
    }
    state.signals_last_60s = sig_count;

    // Build conditions
    std::vector<StrategyCondition> conds;

    // C1: LeaderMove availability
    {
        StrategyCondition c;
        c.name = "LeaderMove";
        c.unit = "count";
        auto it = ctx_.recent_signals.find(SignalKind::LeaderMove);
        c.met = (it != ctx_.recent_signals.end() &&
                 (now - it->second.timestamp) <= cfg_.lag_max_age);
        c.current = c.met ? 1.0 : 0.0;
        c.target = 1.0;
        conds.push_back(c);
    }

    // C2: Correlation
    {
        StrategyCondition c;
        c.name = "Leader corr";
        c.unit = "";
        c.current = frame.leader_correlation;
        c.target = cfg_.min_correlation;
        c.met = (std::abs(frame.leader_correlation) >= cfg_.min_correlation);
        conds.push_back(c);
    }

    // C3: Spread
    {
        StrategyCondition c;
        c.name = "Spread";
        c.unit = "bps";
        double spread_bps = (frame.best_bid > 0 && frame.best_ask > 0)
            ? (frame.best_ask - frame.best_bid) / frame.best_bid * kBpsBase : 999.0;
        c.current = spread_bps;
        c.target = cfg_.max_spread_bps;
        c.met = (spread_bps <= cfg_.max_spread_bps);
        conds.push_back(c);
    }

    // C4: Our change 2s
    {
        StrategyCondition c;
        c.name = "Our change 2s";
        c.unit = "%";
        double our_change_2s = frame.price_change_1s +
                               (frame.price_change_5s - frame.price_change_1s) * 0.25;
        c.current = std::abs(our_change_2s) * 100.0;
        c.target = cfg_.max_our_change_2s_pct;
        c.met = (c.current <= c.target);
        conds.push_back(c);
    }

    // C5: Density on path
    {
        StrategyCondition c;
        c.name = "Density on path";
        c.unit = "bps";
        c.target = cfg_.density_on_path_search_bps;
        c.current = c.target; // default: no blockage
        c.met = true;

        auto it_move = ctx_.recent_signals.find(SignalKind::LeaderMove);
        Side plan_side = Side::Buy;
        if (it_move != ctx_.recent_signals.end()) {
            plan_side = (it_move->second.payload.lag_pct > 0) ? Side::Buy : Side::Sell;
        }
        const double mid_now = frame.mid;
        for (auto rit = ctx_.signal_history.rbegin();
             rit != ctx_.signal_history.rend(); ++rit) {
            if (rit->kind != SignalKind::DensityDetected) continue;
            if ((now - rit->timestamp) > std::chrono::seconds(15)) break;
            const double dist_bps = (rit->price - mid_now) / mid_now * kBpsBase;
            std::string_view side_str = rit->payload.side;
            if (plan_side == Side::Buy) {
                if (side_str == "Ask" && dist_bps > 0.0 &&
                    dist_bps <= cfg_.density_on_path_search_bps) {
                    c.met = false;
                    c.current = dist_bps;
                    break;
                }
            } else {
                if (side_str == "Bid" && dist_bps < 0.0 &&
                    -dist_bps <= cfg_.density_on_path_search_bps) {
                    c.met = false;
                    c.current = -dist_bps;
                    break;
                }
            }
        }
        conds.push_back(c);
    }

    state.conditions = std::move(conds);
    int met = 0;
    for (const auto& c : state.conditions) { if (c.met) ++met; }
    state.readiness_pct = state.conditions.empty() ? 0.0
        : (static_cast<double>(met) / state.conditions.size()) * 100.0;
    state.ready_state = (met == static_cast<int>(state.conditions.size()))
        ? StrategyReadyState::Ready : StrategyReadyState::Warming;

    return state;
}

} // namespace trade_bot
