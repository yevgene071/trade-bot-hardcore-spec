#pragma once

#include "IStrategy.hpp"
#include "StrategyContext.hpp"
#include <chrono>
#include <mutex>

namespace trade_bot {

// Forward declaration for tier-based thresholds
class TickerUniverse;

/**
 * T3-FLUSH: Trades false breakouts ("stop-hunts").
 *
 * Pattern: price breaks a strong S/R level with aggressive tape (TapeFlush),
 * volume fades immediately after, price reverses back through the level.
 * Entry is placed inside the broken level in the reversal direction.
 *
 * Signals consumed: LevelBreak, TapeFlush, TapeBurst (invalidation),
 *                   DensityEating (invalidation).
 */
class FlushReversal : public IStrategy {
public:
    struct Config {
        double max_spread_bps{4.0};
        double entry_offset_bps{2.0};        // entry this far inside the broken level
        double stop_buffer_bps{8.0};         // stop this far beyond the level
        double tp1_r{2.0};                   // flush reversals target 2:1
        double tp1_size_ratio{0.6};

        std::chrono::seconds flush_max_age{15};       // LevelBreak must be this fresh
        std::chrono::seconds tape_flush_max_age{8};   // TapeFlush must be this fresh
        double min_flush_dist_bps{3.0};               // break must penetrate this far past level
        int    min_level_touches{2};                  // level strength gate
        int    min_flush_count{2};                    // Manual rule: trade repeated flushes, not a single print.
        std::chrono::seconds flush_count_window{120}; // window for min_flush_count search

        double max_vol_fade_ratio{0.5};               // vol_1s / avg_vol_1s must be below this
        double min_price_reversal_bps{1.5};           // price must have started reversing

        // Tier-based threshold arrays (tier0, tier1, tier2)
        std::vector<double> tier_min_flush_dist_bps{25.0, 15.0, 10.0};
        std::vector<double> tier_min_price_reversal_bps{8.0, 5.0, 3.0};
        std::vector<double> tier_entry_offset_bps{5.0, 3.0, 2.0};

        std::chrono::seconds entry_timeout{8};
        double no_progress_timeout_sec{30.0};
        double post_entry_grace_sec{3.0};
        double min_follow_through_bps{5.0};
    };

    FlushReversal(Ticker ticker, TickerInfo info, const Config& cfg, std::shared_ptr<IClock> clock = nullptr);
    FlushReversal(Ticker ticker, TickerInfo info, std::shared_ptr<IClock> clock = nullptr);

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
    void reset_active_plan() override;
    void on_plan_accepted(const TradePlan& plan) override;
    std::optional<FixedString<32>> check_close_conditions(const FeatureFrame& latest_frame) override;

    int priority() const override { return 4; }

private:
    Ticker      ticker_;
    TickerInfo  info_;
    std::string name_;
    Config      cfg_;
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
