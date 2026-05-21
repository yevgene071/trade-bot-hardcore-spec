#pragma once

#include "IStrategy.hpp"
#include "StrategyContext.hpp"
#include <chrono>
#include "utils/CircularBuffer.hpp"
#include <mutex>

namespace trade_bot {

// Forward declaration for tier-based thresholds
class TickerUniverse;

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

        // STRATEGIES § 3.4
        double max_our_change_2s_pct{0.1};          // C4: our move must not exceed 0.1%
        double density_on_path_search_bps{25.0};    // C5: scan for density within 25 bps on path

        // Tier-based threshold arrays (tier0, tier1, tier2)
        std::vector<double> tier_max_our_change_2s_pct{0.05, 0.1, 0.15};
        std::vector<double> tier_density_on_path_search_bps{40.0, 30.0, 20.0};
        std::vector<double> tier_stop_distance_bps{12.0, 8.0, 6.0};

        std::chrono::milliseconds lag_max_age{3000};
        std::chrono::seconds entry_timeout{5};

        // Post-entry invalidation (STRATEGIES.md § 3.7)
        double correlation_exit_threshold{0.3};     // exit if correlation drops below
        double leader_exit_reversal_bps{5.0};       // exit if leader reverses >5 bps
        int    swing_lookback_ticks{30};            // ticks for local extremum finder

        // Post-entry timing (STRATEGIES.md § 0.4) — scaled per-ticker in affinity handler
        double no_progress_timeout_sec{15.0};
    };

    LeaderLag(Ticker ticker, TickerInfo info, const Config& cfg, std::shared_ptr<IClock> clock = nullptr);
    LeaderLag(Ticker ticker, TickerInfo info, std::shared_ptr<IClock> clock = nullptr);

    // Set TickerUniverse for tier-based thresholds
    void set_universe(const TickerUniverse* universe) { universe_ = universe; }

    const std::string& name() const override { return name_; }
    const Ticker& ticker() const override { return ticker_; }

    void on_frame(const FeatureFrame& frame) override;
    std::optional<TradePlan> on_signal(const Signal& signal, std::chrono::system_clock::time_point now) override;
    std::optional<TradePlan> tick(std::chrono::system_clock::time_point now) override;
    StrategyState get_state() const override;

    bool has_active_plan() const override {
        std::lock_guard<std::mutex> lk(plan_mtx_);
        return active_plan_.has_value();
    }

    // Post-entry invalidation (STRATEGIES.md § 3.7)
    void on_plan_accepted(const TradePlan& plan) override;
    void reset_active_plan() override;
    std::optional<FixedString<32>> check_close_conditions(const FeatureFrame& latest_frame) override;

    int priority() const override { return 1; }

private:
    // Price history ring buffer for swing-low/high stop placement
    CircularBuffer<double, 120> price_history_;

    [[nodiscard]] double find_swing_low(size_t lookback) const;
    [[nodiscard]] double find_swing_high(size_t lookback) const;

    Ticker          ticker_;
    TickerInfo      info_;
    std::string     name_;
    Config          cfg_;
    StrategyContext ctx_;
    
    // plan_mtx_ guards active_plan_ and active_trade_info_.
    // Lock order: always plan_mtx_ before ctx_.mtx_ to prevent deadlocks.
    mutable std::mutex        plan_mtx_;
    std::optional<TradePlan>  active_plan_;
    std::optional<TradePlan>  active_trade_info_;
    
    // Tier-based thresholds support
    const TickerUniverse* universe_{nullptr};
    
    // P0 DETERMINISM: Clock abstraction for replay (injected via constructor)
    std::shared_ptr<IClock> clock_;
};

} // namespace trade_bot
