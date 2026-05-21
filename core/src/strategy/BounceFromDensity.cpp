#include "BounceFromDensity.hpp"
#include "universe/TickerUniverse.hpp"
#include "logger/Logger.hpp"
#include "numeric/PriceUtils.hpp"
#include <cmath>
#include <algorithm>

namespace trade_bot {

BounceFromDensity::BounceFromDensity(Ticker ticker, TickerInfo info, const Config& cfg, std::shared_ptr<IClock> clock)
    : ticker_(std::move(ticker))
    , info_(std::move(info))
    , name_("BounceFromDensity")
    , cfg_(cfg)
    , clock_(std::move(clock)) {}

BounceFromDensity::BounceFromDensity(Ticker ticker, TickerInfo info, std::shared_ptr<IClock> clock)
    : BounceFromDensity(std::move(ticker), std::move(info), Config{}, std::move(clock)) {}

void BounceFromDensity::on_frame(const FeatureFrame& frame) {
    if (frame.ticker == ticker_) {
        std::lock_guard<std::recursive_mutex> lk(ctx_.mtx_);
        ctx_.update(frame);
    }
}

std::optional<TradePlan> BounceFromDensity::on_signal(const Signal& signal, std::chrono::system_clock::time_point now) {
    if (signal.ticker != ticker_) return std::nullopt;
    
    // Update context with new signal
    {
        std::lock_guard<std::recursive_mutex> ctx_lock(ctx_.mtx_);
        ctx_.update(signal);
    }
    
    // Event-driven: react immediately to LevelApproach signals
    if (signal.kind != SignalKind::LevelApproach) return std::nullopt;
    
    std::lock_guard<std::mutex> plan_lock(plan_mtx_);
    std::lock_guard<std::recursive_mutex> ctx_lock(ctx_.mtx_);
    
    // P0-DETERMINISM: Use injected clock for replay determinism
    if (clock_) now = clock_->now();
    if (active_trade_info_) return std::nullopt; // trade already open — no new entries
    if (active_plan_) return std::nullopt; // plan already pending
    
    // 1. Check conditions C1-C7
    const auto& frame = ctx_.last_frame;

    // C-Spread: reject if spread is too wide (STRATEGIES.md § 1.x)
    if (frame.spread_bps > cfg_.max_spread_bps) {
        LOG_TRACE("[Bounce] {} spread too wide: {:.2f} bps (max {:.2f})", ticker_, frame.spread_bps, cfg_.max_spread_bps);
        return std::nullopt;
    }

    // C1: LevelApproach (most recent)
    auto it_approach = ctx_.recent_signals.find(SignalKind::LevelApproach);
    if (it_approach == ctx_.recent_signals.end() || 
        (now - it_approach->second.timestamp) > cfg_.approach_signal_max_age) return std::nullopt;
    
    double level_price = it_approach->second.price;

    // GAP-02 §1.3: level must be ≤ max_level_age old at time of approach.
    // payload.age_ms = level age at signal emit; add time elapsed since emit.
    {
        auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - it_approach->second.timestamp).count();
        auto total_level_age_ms = it_approach->second.payload.age_ms + static_cast<int>(elapsed_ms);
        if (total_level_age_ms > static_cast<int>(cfg_.max_level_age.count() * 1000)) {
            LOG_TRACE("[Bounce] {} level {:.4f} too old: {}ms (max {}ms)",
                      ticker_, level_price, total_level_age_ms, cfg_.max_level_age.count() * 1000);
            return std::nullopt;
        }
    }

    // §0.8: N-th approach filter — 3rd+ approach = breakout only, skip bounce
    if (cfg_.max_level_touches_for_bounce > 0 &&
        it_approach->second.payload.touches > cfg_.max_level_touches_for_bounce) {
        LOG_TRACE("[Bounce] {} level {:.4f} has {} touches > {} max (3rd+ approach)",
                  ticker_, level_price, it_approach->second.payload.touches, cfg_.max_level_touches_for_bounce);
        return std::nullopt;
    }

    // C2: Impulse approach requirement
    // T3-BOUNCE: Fix #172 - price_change_1s is 0 after 2s of sticky density.
    // Use speed and type from the LevelApproach signal.
    double approach_speed = it_approach->second.payload.speed_bps;
    std::string_view approach_type = it_approach->second.payload.approach_type;

    // Tier-based threshold: use universe_->get_tiered_threshold() if available
    const double min_speed = universe_ 
        ? universe_->get_tiered_threshold(ticker_, cfg_.tier_min_approach_speed_bps_1s)
        : cfg_.min_approach_speed_bps_1s;
    
    if (approach_speed < min_speed && approach_type != "impulse") {
        LOG_TRACE("[Bounce] {} approach speed {:.1f} < {:.1f} (tiered, type: {})", 
                  ticker_, approach_speed, min_speed, approach_type);
        return std::nullopt;
    }

    // C3: DensityDetected on the same level
    auto it_density = std::find_if(ctx_.signal_history.rbegin(), ctx_.signal_history.rend(), [&](const auto& sig) {
        if (sig.kind != SignalKind::DensityDetected) return false;
        if ((now - sig.timestamp) > cfg_.max_level_age) return false;
        double dist_bps = std::abs(sig.price - level_price) / level_price * kBpsBase;
        return dist_bps <= 1.5; // 1.5 bps tolerance
    });
    if (it_density == ctx_.signal_history.rend()) {
        LOG_TRACE("[Bounce] {} no density detected at level {}", ticker_, level_price);
        return std::nullopt;
    }

    // C3b: Density age ≥ configured minimum (STRATEGIES.md § 1.4 — min density maturity)
    // Total age = age at emission (payload.age_ms) + time since emission.
    auto since_emission_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - it_density->timestamp).count();
    auto density_age_ms = it_density->payload.age_ms + static_cast<int>(since_emission_ms);
    if (density_age_ms < cfg_.min_density_age_ms.count()) {
        LOG_TRACE("[Bounce] {} density too young: {}ms (need ≥{}ms)", ticker_, density_age_ms, cfg_.min_density_age_ms.count());
        return std::nullopt;
    }

    // Extract density info for all subsequent checks
    double density_price = it_density->price;
    std::string_view side_str = it_density->payload.side;
    double density_size = it_density->payload.size;
    Side plan_side = (side_str == "Bid") ? Side::Buy : Side::Sell;

    // C-Funding: skip if funding rate is strongly against our direction
    if (cfg_.max_funding_against_bps > 0.0 && frame.funding_rate != 0.0) {
        const double rate_bps = frame.funding_rate * 10000.0;
        bool against = (plan_side == Side::Buy  && rate_bps >  cfg_.max_funding_against_bps) ||
                       (plan_side == Side::Sell && rate_bps < -cfg_.max_funding_against_bps);
        if (against) {
            LOG_TRACE("[Bounce] {} rejected: funding {:.2f} bps against side", ticker_, rate_bps);
            return std::nullopt;
        }
    }

    // C-MarkMid: skip if mark price diverges from mid (exchange price distortion)
    if (cfg_.max_mark_mid_bps > 0.0 && frame.mark_price > 0.0 && frame.mid > 0.0) {
        const double diverge_bps = std::abs(frame.mark_price - frame.mid) / frame.mid * kBpsBase;
        if (diverge_bps > cfg_.max_mark_mid_bps) {
            LOG_TRACE("[Bounce] {} rejected: mark/mid divergence {:.2f} bps", ticker_, diverge_bps);
            return std::nullopt;
        }
    }

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

    // Relative density check (tier-based)
    const double min_rel_density = universe_
        ? universe_->get_tiered_threshold(ticker_, cfg_.tier_min_relative_density)
        : cfg_.min_relative_density;
    
    double book_depth = (side_str == "Bid") ? frame.bid_depth_10 : frame.ask_depth_10;
    if (book_depth > 0 && (density_size / book_depth) < min_rel_density) {
        LOG_TRACE("[Bounce] {} density {:.2f} / depth {:.2f} = {:.3f} < {:.3f} (tiered)", 
                  ticker_, density_size, book_depth, density_size / book_depth, min_rel_density);
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

    // C5: Driver (Leader) alignment (tier-based)
    // "Поводырь: Желательно чтобы шел в сторону отскока / Разворачивался"
    const double min_driver_reversal = universe_
        ? universe_->get_tiered_threshold(ticker_, cfg_.tier_min_driver_reversal_bps)
        : cfg_.min_driver_reversal_bps;
    
    double leader_move = (plan_side == Side::Buy) ? frame.leader_change_1s : -frame.leader_change_1s;
    // T0-BUGFIX: Removed division by 100 — leader_move is a fractional decimal,
    // kBpsBase=10000 converts to bps. Previously /100 turned bps→percent, which
    // never exceeded min_driver_reversal_bps (nominally 2 bps), making this check dead.
    if (leader_move < 0 && std::abs(leader_move * kBpsBase) > min_driver_reversal) {
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

    // §5.6: Density cluster — scan for additional same-side densities within cluster range.
    // Entry anchors to the first (nearest) density; stop anchors beyond the furthest.
    double stop_anchor_price = density_price;
    if (cfg_.density_cluster_enabled) {
        double far_price = density_price;
        for (auto rit = ctx_.signal_history.rbegin(); rit != ctx_.signal_history.rend(); ++rit) {
            if (rit->kind != SignalKind::DensityDetected) continue;
            if ((now - rit->timestamp) > cfg_.max_level_age) continue;
            if (rit->payload.side != side_str) continue;
            double dist_bps = std::abs(rit->price - density_price) / density_price * kBpsBase;
            if (dist_bps > cfg_.density_cluster_max_bps || dist_bps < 0.5) continue; // skip primary itself
            if (plan_side == Side::Buy  && rit->price < far_price) far_price = rit->price;
            if (plan_side == Side::Sell && rit->price > far_price) far_price = rit->price;
        }
        if (far_price != density_price) {
            LOG_INFO("[Bounce] {} density cluster: first={:.4f} far={:.4f} — stop anchored to far density",
                     ticker_, density_price, far_price);
            stop_anchor_price = far_price;
        }
    }

    // Calculate Prices
    // Entry anchors to first (nearest) density; stop goes beyond stop_anchor_price (may be cluster far).
    double entry_price, stop_price;
    double offset = level_price * (cfg_.entry_offset_bps / kBpsBase);
    double buffer = stop_anchor_price * (cfg_.stop_buffer_bps / kBpsBase);

    if (plan_side == Side::Buy) {
        entry_price = level_price + offset;
        stop_price = stop_anchor_price - buffer;  // below the furthest support density
    } else {
        entry_price = level_price - offset;
        stop_price = stop_anchor_price + buffer;  // above the furthest resistance density
    }

    entry_price = round_to_tick(entry_price, info_.price_increment);
    stop_price = round_to_tick(stop_price, info_.price_increment);

    // C-Anchor: §1.8 — a level must anchor the stop within stop_anchor_max_bps
    if (cfg_.stop_anchor_max_bps > 0.0) {
        bool has_anchor = false;
        if (plan_side == Side::Buy && frame.nearest_support.has_value()) {
            double dist = std::abs(*frame.nearest_support - stop_price) / stop_price * kBpsBase;
            has_anchor = (dist <= cfg_.stop_anchor_max_bps);
        } else if (plan_side == Side::Sell && frame.nearest_resistance.has_value()) {
            double dist = std::abs(*frame.nearest_resistance - stop_price) / stop_price * kBpsBase;
            has_anchor = (dist <= cfg_.stop_anchor_max_bps);
        } else {
            has_anchor = true; // LevelDetector not yet populated — skip check
        }
        if (!has_anchor) {
            LOG_TRACE("[Bounce] {} no anchor within {:.0f} bps of stop {:.4f}", ticker_, cfg_.stop_anchor_max_bps, stop_price);
            return std::nullopt;
        }
    }

    double risk_per_coin = std::abs(entry_price - stop_price);
    if (risk_per_coin <= 0.0) {
        LOG_TRACE("[Bounce] {} zero risk (entry==stop after rounding), skip", ticker_);
        return std::nullopt;
    }
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
        .no_progress_timeout_sec = cfg_.no_progress_timeout_sec,
        .min_follow_through_bps = 0.0,  // M-06: Bounce uses no_progress_timeout, not follow-through
        .density_price_for_stop = density_price,
        .approach_count = it_approach->second.payload.touches,
        .frame_at_entry = {} // Will be set below
    };
    plan.trace_id = plan.evidence.empty() ? 0 : plan.evidence.back().trigger_trace_id;
    
    // P0 DETERMINISM: Use frame snapshot from history matching the trigger trace_id
    if (plan.trace_id != 0) {
        auto historical_frame = ctx_.get_frame_by_trace_id(plan.trace_id);
        if (historical_frame) {
            plan.frame_at_entry = *historical_frame;
            // Debug assertion: verify frame snapshot matches trigger
            if (plan.frame_at_entry.derived_from != plan.trace_id) {
                LOG_ERROR("[Bounce] {}: frame_at_entry.derived_from ({}) != plan.trace_id ({})",
                          ticker_, plan.frame_at_entry.derived_from, plan.trace_id);
            }
        } else {
            LOG_WARN("[Bounce] {}: No frame snapshot found for trace_id {}, using current frame",
                     ticker_, plan.trace_id);
            plan.frame_at_entry = frame;
        }
    } else {
        plan.frame_at_entry = frame;
    }

    active_plan_ = plan;
    return plan;
}

std::optional<TradePlan> BounceFromDensity::tick(std::chrono::system_clock::time_point now) {
    std::lock_guard<std::mutex> plan_lock(plan_mtx_);
    
    // P0-DETERMINISM: Use injected clock for replay determinism
    if (clock_) now = clock_->now();
    
    // TTL check for pending plans
    if (active_plan_ && now > active_plan_->valid_until) {
        LOG_DEBUG("[Bounce] {} plan expired (TTL)", ticker_);
        active_plan_ = std::nullopt;
    }
    
    // All entry logic moved to on_signal() for event-driven processing.
    // tick() only handles TTL cleanup.
    return std::nullopt;
}

bool BounceFromDensity::has_active_plan() const {
    std::lock_guard<std::mutex> lock(plan_mtx_);
    return active_plan_.has_value();
}

void BounceFromDensity::on_plan_accepted(const TradePlan& plan) {
    if (plan.strategy_name == name_) {
        std::lock_guard<std::mutex> lock(plan_mtx_);
        active_trade_info_ = plan;
        LOG_INFO("[Bounce] {}: trade accepted, beginning post-entry monitoring (density_price_for_stop={:.2f})",
                 ticker_, plan.density_price_for_stop);
    }
}

void BounceFromDensity::reset_active_plan() {
    std::lock_guard<std::mutex> lock(plan_mtx_);
    active_plan_ = std::nullopt;
    active_trade_info_ = std::nullopt;
    LOG_INFO("[Bounce] {}: plan rejected/reset, post-entry monitoring cleared", ticker_);
}

std::optional<FixedString<32>> BounceFromDensity::check_close_conditions(const FeatureFrame&) {
    std::lock_guard<std::mutex> plan_lock(plan_mtx_);
    std::lock_guard<std::recursive_mutex> ctx_lock(ctx_.mtx_);
    if (!active_trade_info_) return std::nullopt;
    const auto& plan = *active_trade_info_;
    // P0-DETERMINISM: Use injected clock for replay determinism
    auto now = clock_ ? clock_->now() : std::chrono::system_clock::now();

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
    std::lock_guard<std::mutex> plan_lock(plan_mtx_);
    std::lock_guard<std::recursive_mutex> ctx_lock(ctx_.mtx_);
    StrategyState state;
    state.ticker        = ticker_;
    state.strategy_name = "BounceFromDensity";

    if (active_plan_) {
        state.ready_state = StrategyReadyState::Planning;
        state.readiness_pct = 100.0;
        return state;
    }

    const auto& frame = ctx_.last_frame;
    // P0-DETERMINISM: Use injected clock for replay determinism
    const auto now = clock_ ? clock_->now() : std::chrono::system_clock::now();

    if (frame.ticker.empty()) {
        state.ready_state = StrategyReadyState::Cold;
        state.readiness_pct = 0.0;
        return state;
    }

    state.signals_last_60s = ctx_.recent_signal_count_locked(now, std::chrono::seconds(60));

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
        if (level_price > 0.0) { // M-02: guard against division by zero
            for (auto rit = ctx_.signal_history.rbegin();
                 rit != ctx_.signal_history.rend(); ++rit) {
                if (rit->kind != SignalKind::DensityDetected) continue;
                if ((now - rit->timestamp) > cfg_.max_level_age) break;
                double dist = std::abs(rit->price - level_price) / level_price * kBpsBase;
                if (dist <= 1.5) { c.current = 1.0; break; }
            }
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
                    (now - sig.timestamp) <= cfg_.max_level_age;
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
        bool density_fresh = it_d != ctx_.recent_signals.end() &&
                             (now - it_d->second.timestamp) <= cfg_.max_level_age;
        std::string_view side_str = density_fresh ? it_d->second.payload.side : "";
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
        if (level_price > 0.0) { // M-02: guard against division by zero
            for (auto rit = ctx_.signal_history.rbegin();
                 rit != ctx_.signal_history.rend(); ++rit) {
                if (rit->kind != SignalKind::IcebergSuspected) continue;
                if ((now - rit->timestamp) > std::chrono::seconds(30)) break;
                double dist = std::abs(rit->price - level_price) / level_price * kBpsBase;
                if (dist <= 2.0) { c.current = 1.0; break; }
            }
        }
        c.met = (c.current < 1.0);
        conds.push_back(c);
    }

    // C-Funding: funding rate vs our planned side
    {
        StrategyCondition c;
        c.name = "Funding rate";
        c.unit = "bps";
        c.target = cfg_.max_funding_against_bps;
        c.met = true;
        c.current = 0.0;
        if (cfg_.max_funding_against_bps > 0.0 && frame.funding_rate != 0.0) {
            auto it_d = ctx_.recent_signals.find(SignalKind::DensityDetected);
            bool density_fresh = it_d != ctx_.recent_signals.end() &&
                                 (now - it_d->second.timestamp) <= cfg_.max_level_age;
            std::string_view side_str = density_fresh ? it_d->second.payload.side : "";
            Side plan_side_c = (side_str == "Bid") ? Side::Buy : Side::Sell;
            const double rate_bps = frame.funding_rate * 10000.0;
            c.current = std::abs(rate_bps);
            bool against = (plan_side_c == Side::Buy  && rate_bps >  cfg_.max_funding_against_bps) ||
                           (plan_side_c == Side::Sell && rate_bps < -cfg_.max_funding_against_bps);
            c.met = !against;
        }
        conds.push_back(c);
    }

    // C-MarkMid: mark/mid divergence
    {
        StrategyCondition c;
        c.name = "Mark/mid divergence";
        c.unit = "bps";
        c.target = cfg_.max_mark_mid_bps;
        c.met = true;
        if (cfg_.max_mark_mid_bps > 0.0 && frame.mark_price > 0.0 && frame.mid > 0.0) {
            c.current = std::abs(frame.mark_price - frame.mid) / frame.mid * kBpsBase;
            c.met = (c.current <= cfg_.max_mark_mid_bps);
        } else {
            c.current = 0.0;
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
