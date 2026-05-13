#pragma once

#include "IStrategy.hpp"
#include "signals/SignalBus.hpp"

#include <memory>
#include <vector>
#include <functional>
#include <unordered_map>

namespace trade_bot {

/**
 * Market regime classification (STRATEGIES.md § 6).
 * 3-state classifier: Trend / Range / News.
 */
enum class MarketRegime {
    Unknown,
    Trend,
    Range,
    News
};

/**
 * T3-PLAN: Core engine that manages active strategies and routes events.
 */
class StrategyEngine {
public:
    using PlanCallback = std::function<void(const TradePlan&)>;

    explicit StrategyEngine(SignalBus& signal_bus);

    void add_strategy(std::unique_ptr<IStrategy> strategy);

    /// Removes all strategies for a specific ticker and strategy name.
    void remove_strategy(const Ticker& ticker, const std::string& name);

    /// Routes a feature frame to all strategies.
    void on_frame(const FeatureFrame& frame);

    /// Main loop tick. Triggers strategy logic and collects plans.
    void tick(std::chrono::system_clock::time_point now);

    /// Resets the active plan for a specific strategy (e.g., after risk rejection).
    void reset_strategy_plan(const Ticker& ticker, const std::string& strategy_name);

    /// Notifies the producing strategy that its plan was accepted by risk & executor.
    /// Enables post-entry invalidation monitoring (STRATEGIES.md § 0.4, § 3.7).
    void notify_plan_accepted(const TradePlan& plan);

    /// Callback for when a new trade plan is generated.
    void set_on_plan(PlanCallback cb);

    /// Callback for when a strategy requests an active trade be closed.
    using CloseCallback = std::function<void(const Ticker&, const FixedString<32>&)>;
    void set_close_callback(CloseCallback cb);

    /// Returns the readiness state of all strategies for dashboard display.
    [[nodiscard]] std::vector<StrategyState> get_all_states() const;

    /// Classify the current market regime based on observed features.
    /// Fallback from full HMM (STRATEGIES.md § 6): naive threshold-based classifier.
    [[nodiscard]] MarketRegime classify_regime(const FeatureFrame& frame) const;

    /// Returns the last classified regime for the given ticker.
    [[nodiscard]] MarketRegime current_regime(const Ticker& ticker) const;

private:
    SignalBus& bus_;
    std::unordered_map<Ticker, std::vector<std::unique_ptr<IStrategy>>> ticker_strategies_;
    std::vector<std::unique_ptr<IStrategy>> global_strategies_;
    PlanCallback on_plan_;
    CloseCallback close_cb_;

    // Stored regime per ticker, updated on each tick() via on_frame().
    std::unordered_map<Ticker, MarketRegime> regimes_;
};

} // namespace trade_bot
