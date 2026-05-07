#include "StrategyEngine.hpp"

namespace trade_bot {

StrategyEngine::StrategyEngine(SignalBus& signal_bus)
    : bus_(signal_bus) {
    
    // Subscribe to all signals from the bus and route them to strategies
    bus_.subscribe([this](const Signal& s) {
        for (auto& strat : strategies_) {
            strat->on_signal(s);
        }
    });
}

void StrategyEngine::add_strategy(std::unique_ptr<IStrategy> strategy) {
    strategies_.push_back(std::move(strategy));
}

void StrategyEngine::remove_strategy(const Ticker& ticker, const std::string& name) {
    strategies_.erase(
        std::remove_if(strategies_.begin(), strategies_.end(),
                       [&](const auto& s) {
                           return s->ticker() == ticker && s->name() == name;
                       }),
        strategies_.end());
}

void StrategyEngine::on_frame(const FeatureFrame& frame) {
    for (auto& strat : strategies_) {
        strat->on_frame(frame);
    }
}

void StrategyEngine::tick(std::chrono::system_clock::time_point now) {
    for (auto& strat : strategies_) {
        auto plan = strat->tick(now);
        if (plan && on_plan_) {
            on_plan_(*plan);
        }
    }
}

void StrategyEngine::set_on_plan(PlanCallback cb) {
    on_plan_ = std::move(cb);
}

} // namespace trade_bot
