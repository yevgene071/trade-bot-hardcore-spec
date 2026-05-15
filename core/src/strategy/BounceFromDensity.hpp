#pragma once

#include "IStrategy.hpp"
#include "StrategyContext.hpp"
#include <chrono>

namespace trade_bot {

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

        // STRATEGIES § 1.4
        bool   require_tape_fade{true};        // C4: explicit TapeFade evidence on the level side
        std::chrono::seconds tape_fade_max_age{5};
        double leader_contra_max_pct{0.2};     // C6: |leader_change_3s| < 0.2% contra
        std::chrono::seconds leader_contra_lookback{3};

        std::chrono::milliseconds min_density_age_ms{5000}; // C3b: density must be this old to trade

        // Post-entry invalidation (STRATEGIES.md § 1.7)
        // density_removed scan window = plan.no_progress_timeout_sec (no fixed bound per spec)
        std::chrono::seconds burst_contra_exit_sec{5};  // TapeBurst contra within this window after entry

        std::chrono::seconds max_level_age{60};
        std::chrono::seconds entry_timeout{10};

        // Post-entry timing (STRATEGIES.md § 0.4) — scaled per-ticker in affinity handler
        double no_progress_timeout_sec{120.0};
    };

    BounceFromDensity(Ticker ticker, TickerInfo info, const Config& cfg);
    BounceFromDensity(Ticker ticker, TickerInfo info);

    const std::string& name() const override { return name_; }
    const Ticker& ticker() const override { return ticker_; }

    void on_frame(const FeatureFrame& frame) override;
    void on_signal(const Signal& signal) override;
    std::optional<TradePlan> tick(std::chrono::system_clock::time_point now) override;
    StrategyState get_state() const override;

    bool has_active_plan() const override { return active_plan_.has_value(); }

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
    
    std::optional<TradePlan> active_plan_;
    std::optional<TradePlan> active_trade_info_;
};

} // namespace trade_bot
