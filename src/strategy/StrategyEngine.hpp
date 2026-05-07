#pragma once

#include "IStrategy.hpp"
#include "signals/SignalBus.hpp"

#include <memory>
#include <vector>
#include <functional>

namespace trade_bot {

/**
 * T3-PLAN: Core engine that manages active strategies and routes events.
 */
class StrategyEngine {
public:
    using PlanCallback = std::function<void(const TradePlan&)>;

    StrategyEngine(SignalBus& signal_bus);

    void add_strategy(std::unique_ptr<IStrategy> strategy);

    /// Removes all strategies for a specific ticker and strategy name.
    void remove_strategy(const Ticker& ticker, const std::string& name);

    /// Routes a feature frame to all strategies.
    void on_frame(const FeatureFrame& frame);

    /// Main loop tick. Triggers strategy logic and collects plans.
    void tick(std::chrono::system_clock::time_point now);

    /// Callback for when a new trade plan is generated.
    void set_on_plan(PlanCallback cb);

private:
    SignalBus& bus_;
    std::vector<std::unique_ptr<IStrategy>> strategies_;
    PlanCallback on_plan_;
};

} // namespace trade_bot
