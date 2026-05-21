#pragma once

#include "IStrategy.hpp"
#include "StrategyContext.hpp"
#include <chrono>
#include <mutex>

namespace trade_bot {

// Forward declaration for tier-based thresholds
class TickerUniverse;

/**
 * T3-BOUNCE: Strategy that trades bounces from large order book densities.
 */
class BounceFromDensity : public IStrategy {
public:
    struct Config {
        double max_spread_bps{3.0};
        double entry_offset_bps{3.0};
        double stop_buffer_bps{5.0};
        double tp1_r{1.5};
        double tp1_size_ratio{0.5};
        
        // New filters from ОТСКОК СТРАТЕГИЯ.docx
        double min_approach_speed_bps_1s{5.0}; // Impulse requirement
        double max_tape_speed_ratio{0.6};      // Stalling check (1s vol / 5s vol normalized)
        double min_driver_reversal_bps{2.0};   // Driver alignment
        double min_relative_density{2.0};      // Density vs 10-level book depth

        // Tier-based threshold arrays (tier0, tier1, tier2)
        std::vector<double> tier_min_approach_speed_bps_1s{50.0, 15.0, 8.0};
        std::vector<double> tier_min_relative_density{0.25, 0.15, 0.10};
        std::vector<double> tier_min_driver_reversal_bps{30.0, 20.0, 12.0};

        // STRATEGIES § 1.4
        bool   require_tape_fade{true};        // C4: explicit TapeFade evidence on the level side
        std::chrono::seconds approach_signal_max_age{5}; // C1: max age for LevelApproach signal
        std::chrono::seconds tape_fade_max_age{5};
        double leader_contra_max_pct{0.2};     // C6: |leader_change_3s| < 0.2% contra
        std::chrono::seconds leader_contra_lookback{3};

        std::chrono::milliseconds min_density_age_ms{5000}; // C3b: density must be this old to trade
        int max_level_touches_for_bounce{2}; // §0.8: block bounce if level has > N touches (3rd+ approach = breakout only)

        // Post-entry invalidation (STRATEGIES.md § 1.7)
        // density_removed scan window = plan.no_progress_timeout_sec (no fixed bound per spec)
        std::chrono::seconds burst_contra_exit_sec{5};  // TapeBurst contra within this window after entry

        std::chrono::seconds max_level_age{60};
        std::chrono::seconds entry_timeout{10};
        double stop_anchor_max_bps{15.0}; // §1.8: level beyond stop must be within this range

        // Post-entry timing (STRATEGIES.md § 0.4) — scaled per-ticker in affinity handler
        double no_progress_timeout_sec{120.0};

        // §5.6 Density cluster ("завал плотностей")
        bool   density_cluster_enabled{true};
        double density_cluster_max_bps{15.0}; // max spread for cluster qualification

        // Exchange data filters (0 = disabled)
        double max_funding_against_bps{5.0};  // skip if 8h funding > 5 bps against our side
        double max_mark_mid_bps{20.0};        // skip if |mark - mid| > 20 bps (price distortion)
    };

    BounceFromDensity(Ticker ticker, TickerInfo info, const Config& cfg, std::shared_ptr<IClock> clock = nullptr);
    BounceFromDensity(Ticker ticker, TickerInfo info, std::shared_ptr<IClock> clock = nullptr);

    // Set TickerUniverse for tier-based thresholds
    void set_universe(const TickerUniverse* universe) { universe_ = universe; }

    const std::string& name() const override { return name_; }
    const Ticker& ticker() const override { return ticker_; }

    void on_frame(const FeatureFrame& frame) override;
    std::optional<TradePlan> on_signal(const Signal& signal, std::chrono::system_clock::time_point now) override;
    std::optional<TradePlan> tick(std::chrono::system_clock::time_point now) override;
    StrategyState get_state() const override;

    bool has_active_plan() const override;

    // Post-entry invalidation (STRATEGIES.md § 1.7)
    void on_plan_accepted(const TradePlan& plan) override;
    void reset_active_plan() override;
    std::optional<FixedString<32>> check_close_conditions(const FeatureFrame& latest_frame) override;

    int priority() const override { return 3; }

private:
    Ticker          ticker_;
    TickerInfo      info_;
    std::string     name_;
    Config          cfg_;
    StrategyContext ctx_;

    // plan_mtx_ guards active_plan_ and active_trade_info_ explicitly.
    // Lock order: always plan_mtx_ before ctx_.mtx_ to prevent deadlocks.
    //
    // TODO(perf, task-5): Considered replacing plan_mtx_/ctx_.mtx_ with a
    // double-buffered std::atomic<std::shared_ptr<StrategyState>> snapshot to
    // remove locking from on_signal/tick. Deferred intentionally:
    //   1. The mutex is UNCONTENDED on the hot path — processor_thread is the
    //      sole writer; the only reader (dashboard get_state) runs on a slow
    //      cached refresh path (BotAppRun.cpp:218), so the futex fast-path
    //      lock/unlock is ~15-25ns and not a p99 contributor.
    //   2. ctx_.mtx_ guards large mutable buffers (signal_history up to 1024
    //      entries + 16-frame ring). A correct snapshot would deep-copy these
    //      on every on_signal/on_frame, REGRESSING hot-path latency.
    //   3. Touches 5 strategy files + the recent_signal_count_locked() API
    //      contract — high-risk broad refactor for negligible gain.
    mutable std::mutex       plan_mtx_;
    std::optional<TradePlan> active_plan_;
    std::optional<TradePlan> active_trade_info_;
    
    // Tier-based thresholds support
    const TickerUniverse* universe_{nullptr};
    
    // P0 DETERMINISM: Clock abstraction for replay (injected via constructor)
    std::shared_ptr<IClock> clock_;
};

} // namespace trade_bot
