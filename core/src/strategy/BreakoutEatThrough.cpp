#include "BreakoutEatThrough.hpp"
#include "logger/Logger.hpp"
#include "numeric/PriceUtils.hpp"
#include <algorithm>

namespace trade_bot {

BreakoutEatThrough::BreakoutEatThrough(Ticker ticker, TickerInfo info, const Config& cfg)
    : ticker_(std::move(ticker))
    , info_(std::move(info))
    , name_("BreakoutEatThrough")
    , cfg_(cfg) {}

BreakoutEatThrough::BreakoutEatThrough(Ticker ticker, TickerInfo info)
    : BreakoutEatThrough(std::move(ticker), std::move(info), Config{}) {}

void BreakoutEatThrough::on_signal(const Signal& signal) {
    if (signal.ticker == ticker_) {
        ctx_.update(signal);
    }
}

void BreakoutEatThrough::on_frame(const FeatureFrame& frame) {
    if (frame.ticker == ticker_) {
        ctx_.update(frame);
    }
}

std::optional<TradePlan> BreakoutEatThrough::tick(std::chrono::system_clock::time_point now) {
    std::lock_guard<std::mutex> lock(ctx_.mtx_);
    if (active_plan_) {
        if (now > active_plan_->valid_until) active_plan_ = std::nullopt;
        else return std::nullopt; // Already have a plan pending or active
    }

    const auto& frame = ctx_.last_frame;
    if (frame.ticker.empty()) return std::nullopt;

    // C1: Eating & Progress
    auto it_eating = ctx_.recent_signals.find(SignalKind::DensityEating);
    if (it_eating == ctx_.recent_signals.end() || 
        (now - it_eating->second.timestamp) > std::chrono::seconds(2)) return std::nullopt;

    const double density_original_size = it_eating->second.payload.original_size;
    const double density_remaining_size = it_eating->second.payload.size;
    const double density_price = it_eating->second.price;

    if (density_original_size > 0) {
        double ratio = density_remaining_size / density_original_size;
        if (ratio > 0.5) {
            LOG_TRACE("[Breakout] {} eating too early: {}% remaining", ticker_, ratio * 100.0);
            return std::nullopt; // Too early
        }
    }

    std::string_view side_str = it_eating->second.payload.side;
    Side breakout_side = (side_str == "Ask") ? Side::Buy : Side::Sell; // Eat Sell -> Buy move

    // C2: TapeBurst & Aggression
    auto it_burst = ctx_.recent_signals.find(SignalKind::TapeBurst);
    if (it_burst == ctx_.recent_signals.end() ||
        (now - it_burst->second.timestamp) > std::chrono::seconds(3)) {
        LOG_TRACE("[Breakout] {} no recent TapeBurst", ticker_);
        return std::nullopt;
    }
    
    std::string_view burst_side_str = it_burst->second.payload.side;
    if ((breakout_side == Side::Buy && burst_side_str != "Buy") ||
        (breakout_side == Side::Sell && burst_side_str != "Sell")) {
        LOG_TRACE("[Breakout] {} burst side mismatch (need {}, got {})", ticker_, 
                  breakout_side == Side::Buy ? "Buy" : "Sell", burst_side_str);
        return std::nullopt;
    }

    // Tape Aggression check
    double aggression = (breakout_side == Side::Buy) ? frame.tape_aggression : -frame.tape_aggression;
    if (aggression < cfg_.min_tape_aggression) {
        LOG_TRACE("[Breakout] {} low aggression: {}", ticker_, aggression);
        return std::nullopt;
    }

    // C3: Volume participation
    double current_vol = (breakout_side == Side::Buy) ? frame.buy_vol_5s : frame.sell_vol_5s;
    double avg_vol = (breakout_side == Side::Buy) ? (frame.buy_vol_30s / 6.0) : (frame.sell_vol_30s / 6.0);
    if (avg_vol > 0 && (current_vol / avg_vol) < cfg_.min_relative_volume) {
        LOG_TRACE("[Breakout] {} low relative volume: {}", ticker_, current_vol / avg_vol);
        return std::nullopt;
    }

    // C4: Driver (Leader) alignment
    double leader_dir = (breakout_side == Side::Buy) ? frame.leader_change_1s : -frame.leader_change_1s;
    if (frame.leader_correlation > cfg_.min_leader_correlation && leader_dir < 0) {
        LOG_TRACE("[Breakout] {} leader alignment fail: {}", ticker_, leader_dir);
        return std::nullopt;
    }

    // C5: Resistance clusters check
    double resistance_vol = 0.0;
    for (const auto& sig : ctx_.signal_history) {
        if ((now - sig.timestamp) > std::chrono::seconds(10)) continue;
        if (sig.kind == SignalKind::DensityDetected) { 
             std::string_view s = sig.payload.side;
             bool is_resistance = (breakout_side == Side::Buy && s == "Ask" && sig.price > density_price) ||
                                  (breakout_side == Side::Sell && s == "Bid" && sig.price < density_price);
             if (is_resistance) {
                 resistance_vol += sig.payload.size;
             }
        }
    }
    if (density_original_size > 0 && (resistance_vol / density_original_size) > cfg_.max_resistance_cluster_ratio) {
        LOG_TRACE("[Breakout] {} resistance cluster ratio too high: {}", ticker_, resistance_vol / density_original_size);
        return std::nullopt;
    }

    // C-Invalidation: DensityEating stopped (fully eaten)
    if (density_remaining_size <= 0.0 && density_original_size > 0) {
        LOG_TRACE("[Breakout] {} rejected: density fully eaten, no more progress", ticker_);
        return std::nullopt;
    }

    // C-Invalidation: TapeFade on the breakout side (STRATEGIES.md § 2.7)
    {
        auto it_fade = ctx_.recent_signals.find(SignalKind::TapeFade);
        if (it_fade != ctx_.recent_signals.end() &&
            (now - it_fade->second.timestamp) <= std::chrono::seconds(3)) {
            std::string_view fade_side = it_fade->second.payload.side;
            // TapeFade contra means: the fade is on OUR breakout side,
            // i.e., the aggression we're riding is dying out.
            // E.g., Buy breakout + TapeFade on Buy side = bad (buyers fading).
            // Buy breakout + TapeFade on Sell side = good (sellers fading).
            bool fade_contra = (breakout_side == Side::Buy && fade_side == "Buy") ||
                              (breakout_side == Side::Sell && fade_side == "Sell");
            if (fade_contra) {
                LOG_TRACE("[Breakout] {} rejected: TapeFade on breakout side ({})", ticker_, fade_side);
                return std::nullopt;
            }
        }
    }

    // C-Invalidation: LeaderMove against breakout direction (STRATEGIES.md § 2.7)
    {
        auto it_lm = ctx_.recent_signals.find(SignalKind::LeaderMove);
        if (it_lm != ctx_.recent_signals.end() &&
            (now - it_lm->second.timestamp) <= std::chrono::seconds(3)) {
            double lag_pct = it_lm->second.payload.lag_pct;
            bool lm_contra = (breakout_side == Side::Buy && lag_pct < 0) ||
                            (breakout_side == Side::Sell && lag_pct > 0);
            if (std::abs(lag_pct) > 0.001 && lm_contra) {
                LOG_TRACE("[Breakout] {} rejected: LeaderMove contra {:.4f}%",
                          ticker_, lag_pct * 100.0);
                return std::nullopt;
            }
        }
    }

    // C6: Support behind
    auto it_support = std::find_if(ctx_.signal_history.rbegin(), ctx_.signal_history.rend(), [&](const auto& sig) {
        if ((now - sig.timestamp) > std::chrono::seconds(30)) return false;
        
        bool is_support = false;
        if (sig.kind == SignalKind::DensityDetected) {
            std::string_view s = sig.payload.side;
            if ((breakout_side == Side::Buy && s == "Bid") ||
                (breakout_side == Side::Sell && s == "Ask")) is_support = true;
        } else if (sig.kind == SignalKind::IcebergSuspected) {
             std::string_view s = sig.payload.side;
             if ((breakout_side == Side::Buy && s == "Bid") ||
                 (breakout_side == Side::Sell && s == "Ask")) is_support = true;
        }
        
        if (is_support) {
            double dist_bps = std::abs(sig.price - density_price) / density_price * kBpsBase;
            return dist_bps <= cfg_.support_search_range_bps;
        }
        return false;
    });
    
    if (it_support == ctx_.signal_history.rend()) {
        LOG_TRACE("[Breakout] {} no support found behind", ticker_);
        return std::nullopt;
    }

    double support_price = it_support->price;

    // C5-distance (STRATEGIES § 2.4): density must not already be at the best
    // — if the move started without us, we are too late. Reject when the
    // distance from current best price to density is below min_distance_from_best_bps.
    {
        const double ref_best = (breakout_side == Side::Buy) ? frame.best_ask : frame.best_bid;
        if (ref_best > 0.0) {
            const double dist_bps = std::abs(density_price - ref_best) / ref_best * kBpsBase;
            if (dist_bps < cfg_.min_distance_from_best_bps) {
                LOG_TRACE("[Breakout] {} rejected: density too close to best ({:.2f} bps)",
                          ticker_, dist_bps);
                return std::nullopt;
            }
        }
    }

    LOG_INFO("[Strategy] BreakoutEatThrough TRIGGERED for {}! Submitting plan.", ticker_);

    // Prices
    double entry_price = (breakout_side == Side::Buy) ? density_price + density_price * (cfg_.aggressive_offset_bps / kBpsBase)
                                                    : density_price - density_price * (cfg_.aggressive_offset_bps / kBpsBase);
    entry_price = round_to_tick(entry_price, info_.price_increment);

    double stop_price = (breakout_side == Side::Buy) ? support_price - support_price * (cfg_.stop_buffer_bps / kBpsBase)
                                                    : support_price + support_price * (cfg_.stop_buffer_bps / kBpsBase);
    stop_price = round_to_tick(stop_price, info_.price_increment);

    double risk_per_coin = std::abs(entry_price - stop_price);
    double tp1_price = (breakout_side == Side::Buy) ? entry_price + cfg_.tp1_r * risk_per_coin 
                                                   : entry_price - cfg_.tp1_r * risk_per_coin;
    tp1_price = round_to_tick(tp1_price, info_.price_increment);

    TradePlan plan {
        .ticker = ticker_,
        .side = breakout_side,
        .entry_type = OrderType::Market,
        .entry_price = entry_price,
        .stop_price = stop_price,
        .tp1_price = tp1_price,
        .tp2_price = std::nullopt,
        .tp1_size_ratio = cfg_.tp1_size_ratio,
        .size_coin = 0.0,
        .risk_usd = 0.0,
        .strategy_name = "BreakoutEatThrough",
        .reason = FixedString<128>::format("Breakout through %.*s density", (int)side_str.length(), side_str.data()),
        .evidence = {it_eating->second, it_burst->second},
        .valid_until = now + cfg_.entry_timeout,
        .no_progress_timeout_sec = cfg_.no_progress_timeout_sec,
        .post_entry_grace_sec = cfg_.post_entry_grace_sec,
        .min_follow_through_bps = cfg_.min_follow_through_bps
    };

    active_plan_ = plan;
    return plan;
}

void BreakoutEatThrough::on_plan_accepted(const TradePlan& plan) {
    if (plan.strategy_name == name_) {
        active_trade_info_ = plan;
        LOG_INFO("[Breakout] {}: trade accepted, beginning post-entry monitoring (follow-through check)",
                 ticker_);
    }
}

void BreakoutEatThrough::reset_active_plan() {
    active_plan_ = std::nullopt;
    active_trade_info_ = std::nullopt;
    LOG_INFO("[Breakout] {}: plan rejected/reset, post-entry monitoring cleared", ticker_);
}

std::optional<FixedString<32>> BreakoutEatThrough::check_close_conditions(const FeatureFrame&) {
    if (!active_trade_info_) return std::nullopt;
    const auto& plan = *active_trade_info_;
    auto now = std::chrono::system_clock::now();

    // STRATEGIES.md § 2.7: TapeFade on our breakout side — aggression dying out
    // If the side we bet on is fading, the breakout momentum is lost.
    {
        auto it_fade = ctx_.recent_signals.find(SignalKind::TapeFade);
        if (it_fade != ctx_.recent_signals.end() &&
            (now - it_fade->second.timestamp) <= cfg_.fade_contra_exit_sec) {
            std::string_view fade_side = it_fade->second.payload.side;
            bool fade_on_our_side = (plan.side == Side::Buy && fade_side == "Buy") ||
                                    (plan.side == Side::Sell && fade_side == "Sell");
            if (fade_on_our_side) {
                LOG_WARN("[Breakout] {} TapeFade on breakout side ({}) after entry — requesting close",
                         ticker_, fade_side);
                active_trade_info_ = std::nullopt;
                return FixedString<32>("FadeOnOurSidePostEntry");
            }
        }
    }

    // STRATEGIES.md § 2.7: LeaderMove against breakout direction
    // Leader reversing undermines the breakout thesis.
    {
        auto it_lm = ctx_.recent_signals.find(SignalKind::LeaderMove);
        if (it_lm != ctx_.recent_signals.end() &&
            (now - it_lm->second.timestamp) <= cfg_.leader_contra_exit_sec) {
            double lag_pct = it_lm->second.payload.lag_pct;
            bool lm_contra = (plan.side == Side::Buy && lag_pct < 0) ||
                             (plan.side == Side::Sell && lag_pct > 0);
            if (std::abs(lag_pct) > cfg_.leader_exit_contra_pct && lm_contra) {
                LOG_WARN("[Breakout] {} LeaderMove contra ({:.4f}%) after entry — requesting close",
                         ticker_, lag_pct * 100.0);
                active_trade_info_ = std::nullopt;
                return FixedString<32>("LeaderContraPostEntry");
            }
        }
    }

    return std::nullopt;
}

StrategyState BreakoutEatThrough::get_state() const {
    std::lock_guard<std::mutex> lock(ctx_.mtx_);
    StrategyState state;
    state.ticker        = ticker_;
    state.strategy_name = "BreakoutEatThrough";

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

    int sig_count = 0;
    for (const auto& sig : ctx_.signal_history) {
        if ((now - sig.timestamp) <= std::chrono::seconds(60)) ++sig_count;
    }
    state.signals_last_60s = sig_count;

    std::vector<StrategyCondition> conds;

    // C1: DensityEating availability
    {
        StrategyCondition c;
        c.name = "DensityEating";
        c.unit = "count";
        auto it = ctx_.recent_signals.find(SignalKind::DensityEating);
        c.met = (it != ctx_.recent_signals.end() &&
                 (now - it->second.timestamp) <= std::chrono::seconds(2));
        c.current = c.met ? 1.0 : 0.0;
        c.target = 1.0;
        conds.push_back(c);
    }

    // C2: Eating progress
    {
        StrategyCondition c;
        c.name = "Eating progress";
        c.unit = "ratio";
        auto it = ctx_.recent_signals.find(SignalKind::DensityEating);
        if (it != ctx_.recent_signals.end()) {
            double orig = it->second.payload.original_size;
            double rem = it->second.payload.size;
            c.current = (orig > 0) ? (rem / orig) : 1.0;
        } else {
            c.current = 1.0;
        }
        c.target = 0.5;
        c.met = (c.current <= c.target);
        conds.push_back(c);
    }

    // C3: TapeBurst
    {
        StrategyCondition c;
        c.name = "TapeBurst";
        c.unit = "count";
        auto it = ctx_.recent_signals.find(SignalKind::TapeBurst);
        c.met = (it != ctx_.recent_signals.end() &&
                 (now - it->second.timestamp) <= std::chrono::seconds(3));
        c.current = c.met ? 1.0 : 0.0;
        c.target = 1.0;
        conds.push_back(c);
    }

    // C4: Tape aggression
    {
        StrategyCondition c;
        c.name = "Tape aggression";
        c.unit = "";
        c.current = std::abs(frame.tape_aggression);
        c.target = cfg_.min_tape_aggression;
        c.met = (c.current >= c.target);
        conds.push_back(c);
    }

    // C5: Relative volume
    {
        StrategyCondition c;
        c.name = "Relative volume";
        c.unit = "ratio";
        double avg_vol = (frame.buy_vol_30s + frame.sell_vol_30s) / 6.0;
        double cur_vol = frame.buy_vol_5s + frame.sell_vol_5s;
        c.current = (avg_vol > 0) ? (cur_vol / avg_vol) : 0.0;
        c.target = cfg_.min_relative_volume;
        c.met = (c.current >= c.target);
        conds.push_back(c);
    }

    // C6: Leader alignment
    {
        StrategyCondition c;
        c.name = "Leader alignment";
        c.unit = "";
        double leader_corr = std::abs(frame.leader_correlation);
        c.current = leader_corr;
        c.target = cfg_.min_leader_correlation;
        c.met = (leader_corr >= cfg_.min_leader_correlation);
        conds.push_back(c);
    }

    // C7: Resistance clusters
    {
        StrategyCondition c;
        c.name = "Resist clusters";
        c.unit = "ratio";
        c.target = cfg_.max_resistance_cluster_ratio;
        auto it = ctx_.recent_signals.find(SignalKind::DensityEating);
        double orig_density = it != ctx_.recent_signals.end() ?
            it->second.payload.original_size : 1.0;
        double resist_vol = 0.0;
        double density_price = it != ctx_.recent_signals.end() ?
            it->second.price : 0.0;
        for (const auto& sig : ctx_.signal_history) {
            if ((now - sig.timestamp) > std::chrono::seconds(10)) continue;
            if (sig.kind == SignalKind::DensityDetected) {
                std::string_view s = sig.payload.side;
                if ((s == "Ask" && sig.price > density_price) ||
                    (s == "Bid" && sig.price < density_price)) {
                    resist_vol += sig.payload.size;
                }
            }
        }
        c.current = (orig_density > 0) ? (resist_vol / orig_density) : 0.0;
        c.met = (c.current <= c.target);
        conds.push_back(c);
    }

    // C8: Support presence — only densities/icebergs on the OPPOSITE side
    // of the breakout direction count as support.
    // E.g. breakout through Ask (Buy side) → support = Bid densities behind.
    {
        StrategyCondition c;
        c.name = "Support behind";
        c.unit = "count";
        c.target = 1.0;
        c.current = 0.0;
        auto it_eat = ctx_.recent_signals.find(SignalKind::DensityEating);
        // Determine breakout direction from the DensityEating signal's side
        // Eat Sell (side=="Ask") → Buy breakout. Eat Buy (side=="Bid") → Sell breakout.
        std::string_view eat_side = it_eat != ctx_.recent_signals.end() ?
            it_eat->second.payload.side : "";
        Side breakout_side = (eat_side == "Ask") ? Side::Buy : Side::Sell;
        // Support must be on the opposite side of the breakout direction
        const char* support_side = (breakout_side == Side::Buy) ? "Bid" : "Ask";

        double density_price = it_eat != ctx_.recent_signals.end() ?
            it_eat->second.price : 0.0;
        for (auto rit = ctx_.signal_history.rbegin();
             rit != ctx_.signal_history.rend(); ++rit) {
            if ((now - rit->timestamp) > std::chrono::seconds(30)) break;
            if (rit->kind != SignalKind::DensityDetected &&
                rit->kind != SignalKind::IcebergSuspected) continue;
            std::string_view s = rit->payload.side;
            // Only consider densities on the support side
            if (s != support_side) continue;
            double dist = std::abs(rit->price - density_price) / density_price * kBpsBase;
            if (dist <= cfg_.support_search_range_bps) {
                c.current = 1.0;
                break;
            }
        }
        c.met = (c.current >= 1.0);
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
