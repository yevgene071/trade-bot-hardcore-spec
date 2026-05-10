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

        std::chrono::seconds max_level_age{60};
        std::chrono::seconds entry_timeout{10};
    };

    BounceFromDensity(Ticker ticker, TickerInfo info, const Config& cfg);
    BounceFromDensity(Ticker ticker, TickerInfo info);

    const std::string& name() const override { return name_; }
    const Ticker& ticker() const override { return ticker_; }

    void on_frame(const FeatureFrame& frame) override;
    void on_signal(const Signal& signal) override;
    std::optional<TradePlan> tick(std::chrono::system_clock::time_point now) override;

private:
    Ticker          ticker_;
    TickerInfo      info_;
    std::string     name_;
    Config          cfg_;
    StrategyContext ctx_;
    
    std::optional<TradePlan> active_plan_;
};

} // namespace trade_bot
