#pragma once

#include "IStrategy.hpp"
#include "StrategyContext.hpp"
#include <chrono>
#include <array>

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

        // STRATEGIES § 3.4
        double max_our_change_2s_pct{0.1};          // C4: our move must not exceed 0.1%
        double density_on_path_search_bps{25.0};    // C5: scan for density within 25 bps on path

        std::chrono::milliseconds lag_max_age{3000};
        std::chrono::seconds entry_timeout{5};

        // Post-entry invalidation (STRATEGIES.md § 3.7)
        double correlation_exit_threshold{0.3};     // exit if correlation drops below
        double leader_exit_reversal_bps{5.0};       // exit if leader reverses >5 bps
        int    swing_lookback_ticks{30};            // ticks for local extremum finder
    };

    LeaderLag(Ticker ticker, TickerInfo info, const Config& cfg);
    LeaderLag(Ticker ticker, TickerInfo info);

    const std::string& name() const override { return name_; }
    const Ticker& ticker() const override { return ticker_; }

    void on_frame(const FeatureFrame& frame) override;
    void on_signal(const Signal& signal) override;
    std::optional<TradePlan> tick(std::chrono::system_clock::time_point now) override;
    StrategyState get_state() const override;

    bool has_active_plan() const override { return active_plan_.has_value(); }

    // Post-entry invalidation (STRATEGIES.md § 3.7)
    void on_plan_accepted(const TradePlan& plan) override;
    void reset_active_plan() override;
    std::optional<FixedString<32>> check_close_conditions(const FeatureFrame& latest_frame) override;

    int priority() const override { return 1; }

private:
    // Price history ring buffer for swing-low/high stop placement
    static constexpr size_t kPriceHistorySize = 120;
    std::array<double, kPriceHistorySize> price_history_{};
    size_t price_history_idx_ = 0;
    size_t price_history_count_ = 0;

    [[nodiscard]] double find_swing_low(size_t lookback) const;
    [[nodiscard]] double find_swing_high(size_t lookback) const;

    Ticker          ticker_;
    TickerInfo      info_;
    std::string     name_;
    Config          cfg_;
    StrategyContext ctx_;
    
    std::optional<TradePlan> active_plan_;
    // Trade info for post-entry monitoring (set when executor accepts our plan)
    std::optional<TradePlan> active_trade_info_;
};

} // namespace trade_bot
