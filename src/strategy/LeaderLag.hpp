#pragma once

#include "IStrategy.hpp"
#include "StrategyContext.hpp"
#include <chrono>

namespace trade_bot {

/**
 * T3-LEADERLAG: Strategy that trades when a correlated asset lags behind its leader.
 */
class LeaderLag : public IStrategy {
public:
    struct Config {
        double min_correlation{0.6};
        double max_spread_bps{3.0};
        double stop_distance_bps{8.0};
        double tp1_catchup_ratio{0.7};
        double tp1_size_ratio{0.6};
        
        std::chrono::milliseconds lag_max_age{3000};
        std::chrono::seconds entry_timeout{5};
    };

    LeaderLag(Ticker ticker, const Config& cfg);
    explicit LeaderLag(Ticker ticker);

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
