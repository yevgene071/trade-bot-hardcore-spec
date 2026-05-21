#pragma once

#include "IStrategy.hpp"
#include "StrategyContext.hpp"
#include <chrono>

namespace trade_bot {

// Forward declaration for tier-based thresholds
class TickerUniverse;

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

        // Tier-based threshold arrays (tier0, tier1, tier2)
        std::vector<double> tier_min_tape_aggression{0.35, 0.20, 0.12};
        std::vector<double> tier_min_distance_from_best_bps{8.0, 5.0, 3.0};
        std::vector<double> tier_support_search_range_bps{40.0, 30.0, 20.0};

        std::chrono::seconds entry_timeout{10};

        // Post-entry invalidation (STRATEGIES.md § 2.7)
        std::chrono::seconds fade_contra_exit_sec{5};         // TapeFade on our side within this window → close
        std::chrono::seconds leader_contra_exit_sec{5};       // LeaderMove contra within this window → close
        double leader_exit_contra_pct{0.15};                  // |lag_pct| > 0.15% contra → close

        // Post-entry timing (STRATEGIES.md § 0.4, § 2.7) — scaled per-ticker in affinity handler
        double no_progress_timeout_sec{60.0};
        double post_entry_grace_sec{5.0};
        double min_follow_through_bps{10.0};
    };

    BreakoutEatThrough(Ticker ticker, TickerInfo info, const Config& cfg, std::shared_ptr<IClock> clock = nullptr);
    BreakoutEatThrough(Ticker ticker, TickerInfo info, std::shared_ptr<IClock> clock = nullptr);

    // Set TickerUniverse for tier-based thresholds
    void set_universe(const TickerUniverse* universe) { universe_ = universe; }

    const std::string& name() const override { return name_; }
    const Ticker& ticker() const override { return ticker_; }

    void on_frame(const FeatureFrame& frame) override;
    std::optional<TradePlan> on_signal(const Signal& signal, std::chrono::system_clock::time_point now) override;
    std::optional<TradePlan> tick(std::chrono::system_clock::time_point now) override;
    StrategyState get_state() const override;

    bool has_active_plan() const override {
        std::lock_guard<std::mutex> lock(plan_mtx_);
        return active_plan_.has_value();
    }

    // Post-entry invalidation (STRATEGIES.md § 2.7)
    void on_plan_accepted(const TradePlan& plan) override;
    void reset_active_plan() override;
    std::optional<FixedString<32>> check_close_conditions(const FeatureFrame& latest_frame) override;

    int priority() const override { return 2; }

private:
    Ticker          ticker_;
    TickerInfo      info_;
    std::string     name_;
    Config          cfg_;
    StrategyContext ctx_;
    
    std::optional<TradePlan> active_plan_;
    std::optional<TradePlan> active_trade_info_;
    
    mutable std::mutex       plan_mtx_;
    
    // Tier-based thresholds support
    const TickerUniverse* universe_{nullptr};
    
    // P0 DETERMINISM: Clock abstraction for replay (injected via constructor)
    std::shared_ptr<IClock> clock_;
};

} // namespace trade_bot
