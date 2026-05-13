#include "BounceFromDensity.hpp"
#include "logger/Logger.hpp"
#include "numeric/PriceUtils.hpp"
#include <cmath>
#include <algorithm>

namespace trade_bot {

BounceFromDensity::BounceFromDensity(Ticker ticker, TickerInfo info, const Config& cfg)
    : ticker_(std::move(ticker))
    , info_(std::move(info))
    , name_("BounceFromDensity")
    , cfg_(cfg) {}

BounceFromDensity::BounceFromDensity(Ticker ticker, TickerInfo info)
    : BounceFromDensity(std::move(ticker), std::move(info), Config{}) {}

void BounceFromDensity::on_frame(const FeatureFrame& frame) {
    if (frame.ticker == ticker_) {
        ctx_.update(frame);
    }
}

void BounceFromDensity::on_signal(const Signal& signal) {
    if (signal.ticker == ticker_) {
        ctx_.update(signal);
    }
}

std::optional<TradePlan> BounceFromDensity::tick(std::chrono::system_clock::time_point now) {
    std::lock_guard<std::mutex> lock(ctx_.mtx_);
    if (active_plan_) {
        // TTL check
        if (now > active_plan_->valid_until) {
            active_plan_ = std::nullopt;
        } else {
            return std::nullopt;
        }
    }

    // 1. Check conditions C1-C7
    const auto& frame = ctx_.last_frame;
    
    // C1: LevelApproach (most recent)
    auto it_approach = ctx_.recent_signals.find(SignalKind::LevelApproach);
    if (it_approach == ctx_.recent_signals.end() || 
        (now - it_approach->second.timestamp) > std::chrono::seconds(5)) return std::nullopt;
    
    double level_price = it_approach->second.price;
    
    // C2: Impulse approach requirement
    // T3-BOUNCE: Fix #172 - price_change_1s is 0 after 2s of sticky density.
    // Use speed and type from the LevelApproach signal.
    double approach_speed = it_approach->second.payload.speed_bps;
    std::string_view approach_type = it_approach->second.payload.approach_type;

    if (approach_speed < cfg_.min_approach_speed_bps_1s && approach_type != "impulse") {
        LOG_TRACE("[Bounce] {} approach speed too low: {} (type: {})", ticker_, approach_speed, approach_type);
        return std::nullopt;
    }

    // C3: DensityDetected on the same level
    auto it_density = std::find_if(ctx_.signal_history.rbegin(), ctx_.signal_history.rend(), [&](const auto& sig) {
        if (sig.kind != SignalKind::DensityDetected) return false;
        if ((now - sig.timestamp) > std::chrono::seconds(15)) return false;
        double dist_bps = std::abs(sig.price - level_price) / level_price * kBpsBase;
        return dist_bps <= 1.5; // 1.5 bps tolerance
    });
    if (it_density == ctx_.signal_history.rend()) {
        LOG_TRACE("[Bounce] {} no density detected at level {}", ticker_, level_price);
        return std::nullopt;
    }

    // C3b: Density age ≥ configured minimum (STRATEGIES.md § 1.4 — min density maturity)
    auto density_age_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - it_density->timestamp).count();
    if (density_age_ms < cfg_.min_density_age_ms.count()) {
        LOG_TRACE("[Bounce] {} density too young: {}ms (need ≥{}ms)", ticker_, density_age_ms, cfg_.min_density_age_ms.count());
        return std::nullopt;
    }

    // Extract density info for all subsequent checks
    double density_price = it_density->price;
    std::string_view side_str = it_density->payload.side;
    double density_size = it_density->payload.size;
    Side plan_side = (side_str == "Bid") ? Side::Buy : Side::Sell;

    // C-Invalidation: DensityRemoved on this level (STRATEGIES.md § 1.7)
    {
        auto it_removed = std::find_if(ctx_.signal_history.rbegin(), ctx_.signal_history.rend(), [&](const auto& sig) {
            if (sig.kind != SignalKind::DensityRemoved) return false;
            if ((now - sig.timestamp) > std::chrono::seconds(30)) return false;
            double dist_bps = std::abs(sig.price - density_price) / density_price * kBpsBase;
            return dist_bps <= 1.5;
        });
        if (it_removed != ctx_.signal_history.rend()) {
            LOG_TRACE("[Bounce] {} rejected: density was removed at {:.2f}", ticker_, density_price);
            return std::nullopt;
        }
    }

    // C-Invalidation: TapeBurst against our direction (STRATEGIES.md § 1.7)
    {
        auto it_burst = ctx_.recent_signals.find(SignalKind::TapeBurst);
        if (it_burst != ctx_.recent_signals.end() &&
            (now - it_burst->second.timestamp) <= std::chrono::seconds(3)) {
            std::string_view burst_side = it_burst->second.payload.side;
            bool burst_contra = (plan_side == Side::Buy && burst_side == "Sell") ||
                                (plan_side == Side::Sell && burst_side == "Buy");
            if (burst_contra) {
                LOG_TRACE("[Bounce] {} rejected: TapeBurst opposite direction ({})", ticker_, burst_side);
                return std::nullopt;
            }
        }
    }

    // Relative density check
    double book_depth = (side_str == "Bid") ? frame.bid_depth_10 : frame.ask_depth_10;
    if (book_depth > 0 && (density_size / book_depth) < cfg_.min_relative_density) {
        LOG_TRACE("[Bounce] {} density too small vs depth: {}", ticker_, density_size / book_depth);
        return std::nullopt;
    }

    // C4: Stalling check (Tape Speed Reduction)
    // "Скорость уменьшается... замирания в финале"
    double vol_1s = frame.buy_vol_1s + frame.sell_vol_1s;
    double vol_5s = frame.buy_vol_5s + frame.sell_vol_5s;
    if (vol_5s > 0) {
        double avg_vol_1s = vol_5s / 5.0;
        double speed_ratio = vol_1s / std::max(1.0, avg_vol_1s); // Avoid div by 0 and oversensitivity
        
        if (speed_ratio > cfg_.max_tape_speed_ratio) {
            LOG_TRACE("[Bounce] {} tape speed too high: {} (vol1s={}, vol5s={})", ticker_, speed_ratio, vol_1s, vol_5s);
            return std::nullopt;
        }
    }

    // C5: Driver (Leader) alignment
    // "Поводырь: Желательно чтобы шел в сторону отскока / Разворачивался"
    double leader_move = (plan_side == Side::Buy) ? frame.leader_change_1s : -frame.leader_change_1s;
    // T0-BUGFIX: Removed division by 100 — leader_move is a fractional decimal,
    // kBpsBase=10000 converts to bps. Previously /100 turned bps→percent, which
    // never exceeded min_driver_reversal_bps (nominally 2 bps), making this check dead.
    if (leader_move < 0 && std::abs(leader_move * kBpsBase) > cfg_.min_driver_reversal_bps) {
        // Driver is moving AGAINST the bounce (i.e. still going towards the density)
        LOG_TRACE("[Bounce] {} leader moving against bounce: {}", ticker_, leader_move);
        return std::nullopt;
    }

    // C4-explicit (STRATEGIES § 1.4): TapeFade evidence — aggression toward the level
    // must be visibly fading. The volume-ratio check above is a proxy; this fail-fast
    // check fires when the dedicated TapeAnalyzer never emitted a fade signal on our
    // approach side. Configurable to keep prior behaviour available via flag.
    if (cfg_.require_tape_fade) {
        auto it_fade = ctx_.recent_signals.find(SignalKind::TapeFade);
        if (it_fade == ctx_.recent_signals.end() ||
            (now - it_fade->second.timestamp) > cfg_.tape_fade_max_age) {
            LOG_TRACE("[Bounce] {} rejected: no recent TapeFade", ticker_);
            return std::nullopt;
        }
    }

    // C6-explicit (STRATEGIES § 1.4): no LeaderMove against our direction in the
    // lookback window. We already gate on leader_change_1s; this adds the
    // LeaderMove-signal-level guard (different code path, captures the upstream
    // event even if the rolling change happens to be small at this exact frame).
    {
        auto it_lm = ctx_.recent_signals.find(SignalKind::LeaderMove);
        if (it_lm != ctx_.recent_signals.end() &&
            (now - it_lm->second.timestamp) <= cfg_.leader_contra_lookback) {
            const double lag_pct = it_lm->second.payload.lag_pct;
            // lag_pct > 0 ⇒ leader implies long catch-up; <0 ⇒ short.
            const bool contra = (plan_side == Side::Buy && lag_pct < 0.0) ||
                                (plan_side == Side::Sell && lag_pct > 0.0);
            if (contra && std::abs(lag_pct) * 100.0 > cfg_.leader_contra_max_pct) {
                LOG_TRACE("[Bounce] {} rejected: LeaderMove contra {:.3f}%",
                          ticker_, lag_pct * 100.0);
                return std::nullopt;
            }
        }
    }

    // C-Iceberg: No Iceberg on this level
    auto it_iceberg = std::find_if(ctx_.signal_history.rbegin(), ctx_.signal_history.rend(), [&](const auto& sig) {
        if (sig.kind != SignalKind::IcebergSuspected) return false;
        if ((now - sig.timestamp) > std::chrono::seconds(30)) return false;
        if (sig.payload.side != side_str) return false; // Only if same side
        double dist_bps = std::abs(sig.price - level_price) / level_price * kBpsBase;
        return dist_bps <= 2.0; 
    });
    if (it_iceberg != ctx_.signal_history.rend()) {
        LOG_TRACE("[Bounce] {} rejected due to suspected iceberg on level", ticker_);
        return std::nullopt;
    }

    LOG_INFO("[Strategy] BounceFromDensity TRIGGERED for {}! Submitting plan.", ticker_);

    // Calculate Prices
    // FIX: stop_price is based on density_price, NOT level_price.
    // The stop should be PLACED BEYOND THE DENSITY, not beyond the approach level.
    // This is critical for correct R:R — stop is ~5 bps past the density, not 5 bps
    // past the approach level which could be much further.
    double entry_price, stop_price;
    double offset = level_price * (cfg_.entry_offset_bps / kBpsBase);
    double buffer = density_price * (cfg_.stop_buffer_bps / kBpsBase);

    if (plan_side == Side::Buy) {
        entry_price = level_price + offset;
        stop_price = density_price - buffer;  // below the density (support)
    } else {
        entry_price = level_price - offset;
        stop_price = density_price + buffer;  // above the density (resistance)
    }

    entry_price = round_to_tick(entry_price, info_.price_increment);
    stop_price = round_to_tick(stop_price, info_.price_increment);

    double risk_per_coin = std::abs(entry_price - stop_price);
    double tp1_price = (plan_side == Side::Buy) ? entry_price + cfg_.tp1_r * risk_per_coin 
                                               : entry_price - cfg_.tp1_r * risk_per_coin;
    tp1_price = round_to_tick(tp1_price, info_.price_increment);

    TradePlan plan {
        .ticker = ticker_,
        .side = plan_side,
        .entry_type = OrderType::Limit,
        .entry_price = entry_price,
        .stop_price = stop_price,
        .tp1_price = tp1_price,
        .tp2_price = std::nullopt,
        .tp1_size_ratio = cfg_.tp1_size_ratio,
        .size_coin = 0.0,
        .risk_usd = 0.0,
        .strategy_name = "BounceFromDensity",
        .reason = FixedString<128>::format("Bounce from %.*s density (Impulse + Stalling)", (int)side_str.length(), side_str.data()),
        .evidence = {*it_density, it_approach->second},
        .valid_until = now + cfg_.entry_timeout,
        .no_progress_timeout_sec = 120.0,
        .density_price_for_stop = density_price
    };

    active_plan_ = plan;
    return plan;
}

void BounceFromDensity::on_plan_accepted(const TradePlan& plan) {
    if (plan.strategy_name == name_) {
        active_trade_info_ = plan;
        LOG_INFO("[Bounce] {}: trade accepted, beginning post-entry monitoring (density_price_for_stop={:.2f})",
                 ticker_, plan.density_price_for_stop);
    }
}

void BounceFromDensity::reset_active_plan() {
    active_plan_ = std::nullopt;
    active_trade_info_ = std::nullopt;
    LOG_INFO("[Bounce] {}: plan rejected/reset, post-entry monitoring cleared", ticker_);
}

std::optional<FixedString<32>> BounceFromDensity::check_close_conditions(const FeatureFrame&) {
    if (!active_trade_info_) return std::nullopt;
    const auto& plan = *active_trade_info_;
    auto now = std::chrono::system_clock::now();

    // STRATEGIES.md § 1.7: Density removed before TP1 → close by market
    // Per spec: unbounded scan — scan ALL signal history within no_progress_timeout.
    const double density_price = plan.density_price_for_stop;
    if (density_price > 0.0) {
        const auto age_limit = std::chrono::seconds(static_cast<long>(plan.no_progress_timeout_sec));
        bool density_gone = false;
        for (auto rit = ctx_.signal_history.rbegin(); rit != ctx_.signal_history.rend(); ++rit) {
            if (rit->kind != SignalKind::DensityRemoved) continue;
            if ((now - rit->timestamp) > age_limit) break;
            double dist_bps = std::abs(rit->price - density_price) / density_price * kBpsBase;
            if (dist_bps <= 1.5) { density_gone = true; break; }
        }
        if (density_gone) {
            LOG_WARN("[Bounce] {} density {:.2f} REMOVED before TP1 — requesting close",
                     ticker_, density_price);
            active_trade_info_ = std::nullopt;
            return FixedString<32>("DensityRemovedPostEntry");
        }
    }

    // STRATEGIES.md § 1.7: TapeBurst against our bounce direction
    // If aggressive volume hits the density from the opposite side, the bounce
    // may turn into a breakout — invalidate the trade.
    {
        auto it_burst = ctx_.recent_signals.find(SignalKind::TapeBurst);
        if (it_burst != ctx_.recent_signals.end() &&
            (now - it_burst->second.timestamp) <= cfg_.burst_contra_exit_sec) {
            std::string_view burst_side = it_burst->second.payload.side;
            bool burst_contra = (plan.side == Side::Buy && burst_side == "Sell") ||
                                (plan.side == Side::Sell && burst_side == "Buy");
            if (burst_contra) {
                LOG_WARN("[Bounce] {} TapeBurst contra ({}) after entry — requesting close",
                         ticker_, burst_side);
                active_trade_info_ = std::nullopt;
                return FixedString<32>("BurstContraPostEntry");
            }
        }
    }

    return std::nullopt;
}

StrategyState BounceFromDensity::get_state() const {
    std::lock_guard<std::mutex> lock(ctx_.mtx_);
    StrategyState state;
    state.ticker        = ticker_;
    state.strategy_name = "BounceFromDensity";

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

    // C1: LevelApproach availability
    {
        StrategyCondition c;
        c.name = "LevelApproach";
        c.unit = "count";
        auto it = ctx_.recent_signals.find(SignalKind::LevelApproach);
        c.met = (it != ctx_.recent_signals.end() &&
                 (now - it->second.timestamp) <= std::chrono::seconds(5));
        c.current = c.met ? 1.0 : 0.0;
        c.target = 1.0;
        conds.push_back(c);
    }

    // C2: Approach speed
    {
        StrategyCondition c;
        c.name = "Approach speed";
        c.unit = "bps";
        auto it = ctx_.recent_signals.find(SignalKind::LevelApproach);
        c.current = it != ctx_.recent_signals.end() ?
            it->second.payload.speed_bps : 0.0;
        c.target = cfg_.min_approach_speed_bps_1s;
        std::string_view atype = it != ctx_.recent_signals.end() ?
            it->second.payload.approach_type : "";
        c.met = (c.current >= c.target || atype == "impulse");
        conds.push_back(c);
    }

    // C3: Density at level
    {
        StrategyCondition c;
        c.name = "Density at level";
        c.unit = "count";
        c.target = 1.0;
        c.current = 0.0;
        auto it_approach = ctx_.recent_signals.find(SignalKind::LevelApproach);
        double level_price = it_approach != ctx_.recent_signals.end() ?
            it_approach->second.price : 0.0;
        for (auto rit = ctx_.signal_history.rbegin();
             rit != ctx_.signal_history.rend(); ++rit) {
            if (rit->kind != SignalKind::DensityDetected) continue;
            if ((now - rit->timestamp) > std::chrono::seconds(15)) break;
            double dist = std::abs(rit->price - level_price) / level_price * kBpsBase;
            if (dist <= 1.5) { c.current = 1.0; break; }
        }
        c.met = (c.current >= 1.0);
        conds.push_back(c);
    }

    // C4: Relative density
    {
        StrategyCondition c;
        c.name = "Relative density";
        c.unit = "ratio";
        c.target = cfg_.min_relative_density;
        c.current = 0.0;
        auto it_density = std::find_if(ctx_.signal_history.rbegin(),
            ctx_.signal_history.rend(), [&](const auto& sig) {
                return sig.kind == SignalKind::DensityDetected &&
                    (now - sig.timestamp) <= std::chrono::seconds(15);
            });
        if (it_density != ctx_.signal_history.rend()) {
            std::string_view s = it_density->payload.side;
            double density_size = it_density->payload.size;
            double book_depth = (s == "Bid") ? frame.bid_depth_10 : frame.ask_depth_10;
            c.current = (book_depth > 0) ? (density_size / book_depth) : 0.0;
        }
        c.met = (c.current >= c.target);
        conds.push_back(c);
    }

    // C5: Tape speed ratio (stalling)
    {
        StrategyCondition c;
        c.name = "Tape stalling";
        c.unit = "ratio";
        double vol_1s = frame.buy_vol_1s + frame.sell_vol_1s;
        double vol_5s = frame.buy_vol_5s + frame.sell_vol_5s;
        double avg_vol_1s = (vol_5s > 0) ? (vol_5s / 5.0) : 1.0;
        c.current = vol_1s / std::max(1.0, avg_vol_1s);
        c.target = cfg_.max_tape_speed_ratio;
        c.met = (c.current <= c.target);
        conds.push_back(c);
    }

    // C6: Leader alignment
    {
        StrategyCondition c;
        c.name = "Leader alignment";
        c.unit = "bps";
        c.current = std::abs(frame.leader_change_1s) * kBpsBase;
        c.target = cfg_.min_driver_reversal_bps;
        // More nuanced: leader not strongly against us
        auto it_d = ctx_.recent_signals.find(SignalKind::DensityDetected);
        std::string_view side_str = it_d != ctx_.recent_signals.end() ?
            it_d->second.payload.side : "";
        Side plan_side = (side_str == "Bid") ? Side::Buy : Side::Sell;
        double leader_move = (plan_side == Side::Buy) ?
            frame.leader_change_1s : -frame.leader_change_1s;
        c.met = (leader_move >= 0 || std::abs(leader_move * kBpsBase) <= cfg_.min_driver_reversal_bps);
        conds.push_back(c);
    }

    // C7: TapeFade
    {
        StrategyCondition c;
        c.name = "TapeFade";
        c.unit = "count";
        if (cfg_.require_tape_fade) {
            auto it = ctx_.recent_signals.find(SignalKind::TapeFade);
            c.met = (it != ctx_.recent_signals.end() &&
                     (now - it->second.timestamp) <= cfg_.tape_fade_max_age);
            c.current = c.met ? 1.0 : 0.0;
        } else {
            c.current = 1.0;
            c.met = true;
        }
        c.target = 1.0;
        conds.push_back(c);
    }

    // C8: LeaderMove contra
    {
        StrategyCondition c;
        c.name = "LeaderMove contra";
        c.unit = "%";
        c.target = cfg_.leader_contra_max_pct;
        c.current = 0.0;
        c.met = true;
        auto it_lm = ctx_.recent_signals.find(SignalKind::LeaderMove);
        if (it_lm != ctx_.recent_signals.end() &&
            (now - it_lm->second.timestamp) <= cfg_.leader_contra_lookback) {
            c.current = std::abs(it_lm->second.payload.lag_pct) * 100.0;
            c.met = (c.current <= c.target);
        }
        conds.push_back(c);
    }

    // C9: No Iceberg
    {
        StrategyCondition c;
        c.name = "No Iceberg";
        c.unit = "count";
        c.target = 0.0;
        c.current = 0.0;
        auto it_approach = ctx_.recent_signals.find(SignalKind::LevelApproach);
        double level_price = it_approach != ctx_.recent_signals.end() ?
            it_approach->second.price : 0.0;
        for (auto rit = ctx_.signal_history.rbegin();
             rit != ctx_.signal_history.rend(); ++rit) {
            if (rit->kind != SignalKind::IcebergSuspected) continue;
            if ((now - rit->timestamp) > std::chrono::seconds(30)) break;
            double dist = std::abs(rit->price - level_price) / level_price * kBpsBase;
            if (dist <= 2.0) { c.current = 1.0; break; }
        }
        c.met = (c.current < 1.0);
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
