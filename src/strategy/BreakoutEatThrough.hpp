#pragma once

#include "IStrategy.hpp"
#include "StrategyContext.hpp"
#include <chrono>

namespace trade_bot {

/**
 * T3-BREAKOUT: Strategy that trades breakouts through large order book densities.
 */
class BreakoutEatThrough : public IStrategy {
public:
    struct Config {
        double max_avg_spread_bps{4.0};
        double aggressive_offset_bps{2.0};
        double stop_buffer_bps{5.0};
        double tp1_r{1.5};
        double tp1_size_ratio{0.5};
        
        double support_search_range_bps{20.0};
        double min_distance_from_best_bps{2.0};
        
        // New filters from ФАКТОРЫ СЛАБОГО ПРОБОЯ.docx
        double min_leader_correlation{0.6};   // Driver alignment
        double min_tape_aggression{0.3};      // Tape directionality (buy-sell)/total
        double min_relative_volume{1.5};      // Volume surge vs 30s avg
        double max_resistance_cluster_ratio{0.7}; // Max ratio of resistance clusters vs our entry density

        std::chrono::seconds entry_timeout{10};
    };

    BreakoutEatThrough(Ticker ticker, TickerInfo info, const Config& cfg);
    BreakoutEatThrough(Ticker ticker, TickerInfo info);

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
