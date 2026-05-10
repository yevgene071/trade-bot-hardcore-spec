#include "StrategyEngine.hpp"

namespace trade_bot {

StrategyEngine::StrategyEngine(SignalBus& signal_bus)
    : bus_(signal_bus) {
    
    // T4-PERF: Route signals to interested strategies by ticker (#160)
    bus_.subscribe([this](const Signal& s) {
        if (!s.ticker.empty()) {
            auto it = ticker_strategies_.find(s.ticker);
            if (it != ticker_strategies_.end()) {
                for (auto& strat : it->second) strat->on_signal(s);
            }
        }
        for (auto& strat : global_strategies_) {
            strat->on_signal(s);
        }
    });
}

void StrategyEngine::add_strategy(std::unique_ptr<IStrategy> strategy) {
    if (strategy->ticker().empty()) {
        global_strategies_.push_back(std::move(strategy));
    } else {
        ticker_strategies_[strategy->ticker()].push_back(std::move(strategy));
    }
}

void StrategyEngine::remove_strategy(const Ticker& ticker, const std::string& name) {
    if (ticker.empty()) {
        global_strategies_.erase(
            std::remove_if(global_strategies_.begin(), global_strategies_.end(),
                           [&](const auto& s) { return s->name() == name; }),
            global_strategies_.end());
    } else {
        auto it = ticker_strategies_.find(ticker);
        if (it != ticker_strategies_.end()) {
            it->second.erase(
                std::remove_if(it->second.begin(), it->second.end(),
                               [&](const auto& s) { return s->name() == name; }),
                it->second.end());
            if (it->second.empty()) ticker_strategies_.erase(it);
        }
    }
}

void StrategyEngine::on_frame(const FeatureFrame& frame) {
    auto it = ticker_strategies_.find(frame.ticker);
    if (it != ticker_strategies_.end()) {
        for (auto& strat : it->second) strat->on_frame(frame);
    }
    for (auto& strat : global_strategies_) strat->on_frame(frame);
}

void StrategyEngine::tick(std::chrono::system_clock::time_point now) {
    auto process_tick = [&](auto& list) {
        for (auto& strat : list) {
            auto plan = strat->tick(now);
            if (plan && on_plan_) {
                on_plan_(*plan);
            }
        }
    };

    for (auto& [ticker, list] : ticker_strategies_) {
        process_tick(list);
    }
    process_tick(global_strategies_);
}

void StrategyEngine::set_on_plan(PlanCallback cb) {
    on_plan_ = std::move(cb);
}

} // namespace trade_bot
