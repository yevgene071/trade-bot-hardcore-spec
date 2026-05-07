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
        
        std::chrono::seconds max_level_age{60};
        std::chrono::seconds entry_timeout{10};
    };

    BounceFromDensity(Ticker ticker, Config cfg);
    BounceFromDensity(Ticker ticker);

    const std::string& name() const override { return name_; }
    const Ticker& ticker() const override { return ticker_; }

    void on_frame(const FeatureFrame& frame) override;
    void on_signal(const Signal& signal) override;
    std::optional<TradePlan> tick(std::chrono::system_clock::time_point now) override;

private:
    Ticker          ticker_;
    std::string     name_;
    Config          cfg_;
    StrategyContext ctx_;
    
    std::optional<TradePlan> active_plan_;
};

} // namespace trade_bot
