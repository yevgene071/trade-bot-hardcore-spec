#include "StrategyEngine.hpp"
#include "logger/Logger.hpp"

#include <algorithm>

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
        for (auto& strat : it->second) {
            strat->on_frame(frame);
        }
    }
    for (auto& strat : global_strategies_) strat->on_frame(frame);

    // Post-entry invalidation check (STRATEGIES.md § 3.7)
    // After routing the frame to all strategies, check if any strategy
    // with an active trade requests force-close due to correlation
    // breakdown or leader reversal.
    if (close_cb_) {
        if (it != ticker_strategies_.end()) {
            for (auto& strat : it->second) {
                auto reason = strat->check_close_conditions(frame);
                if (reason) {
                    LOG_WARN("[StrategyEngine] {}: {} requests close — {}",
                             frame.ticker, strat->name(), *reason);
                    close_cb_(strat->ticker(), *reason);
                }
            }
        }
    }

    // Update regime classification for this ticker
    regimes_[frame.ticker] = classify_regime(frame);
}

void StrategyEngine::tick(std::chrono::system_clock::time_point now) {
    // Collect plans grouped by ticker with their strategy priority and pointer.
    // The strategy pointer is needed for fallback: after the on_plan_ callback,
    // we check has_active_plan() to see if risk accepted the plan.
    struct PlanWithPriority {
        int priority;
        TradePlan plan;
        IStrategy* strategy;  // non-owning — pointer into ticker_strategies_ / global_strategies_
    };
    std::unordered_map<Ticker, std::vector<PlanWithPriority>> plans_by_ticker;

    auto collect = [&](auto& list) {
        for (auto& strat : list) {
            auto plan = strat->tick(now);
            if (plan) {
                plans_by_ticker[plan->ticker].push_back({strat->priority(), *plan, strat.get()});
            }
        }
    };

    for (auto& [ticker, list] : ticker_strategies_) {
        collect(list);
    }
    collect(global_strategies_);

    // For each ticker, sort plans by priority and emit the highest-priority one.
    // If the top-priority plan is rejected (strategy's active_plan_ gets cleared
    // synchronously in the on_plan_ callback), fall back to the next plan.
    for (auto& [ticker, plans] : plans_by_ticker) {
        if (plans.empty()) continue;

        // GAP-1 FIX: Suppress plans based on current market regime.
        // News → all entries dangerous (extreme volatility, stops get blown).
        // Trend → BounceFromDensity unreliable (levels get broken in directional moves).
        // Range → LeaderLag lacks momentum signal (leader moves are mean-reverting noise).
        {
            MarketRegime regime = current_regime(ticker);
            if (regime == MarketRegime::News) {
                LOG_DEBUG("[StrategyEngine] {} regime=News — suppressing all plans", ticker);
                continue;
            }
            if (regime == MarketRegime::Trend) {
                plans.erase(std::remove_if(plans.begin(), plans.end(),
                    [](const PlanWithPriority& pw) {
                        return pw.plan.strategy_name == "BounceFromDensity";
                    }), plans.end());
                if (plans.empty()) continue;
            }
            if (regime == MarketRegime::Range) {
                plans.erase(std::remove_if(plans.begin(), plans.end(),
                    [](const PlanWithPriority& pw) {
                        return pw.plan.strategy_name == "LeaderLag";
                    }), plans.end());
                if (plans.empty()) continue;
            }
        }

        // Sort ascending by priority (lower number = higher priority)
        std::sort(plans.begin(), plans.end(),
                  [](const PlanWithPriority& a, const PlanWithPriority& b) {
                      return a.priority < b.priority;
                  });

        if (!on_plan_) continue;

        // Try plans in priority order until one is accepted (the strategy's
        // active_plan_ remains set after acceptance) or we run out of plans.
        // The on_plan_ callback calls risk_manager_->evaluate(); if rejected,
        // it calls reset_strategy_plan() which clears active_plan_ on the
        // originating strategy, so has_active_plan() returns false.
        bool accepted = false;
        for (auto& pw : plans) {
            on_plan_(pw.plan);
            // After the synchronous callback, check if the strategy still has
            // its active_plan_ set (risk accepted it). If yes, we're done.
            // If not, the plan was rejected — try the next priority.
            if (pw.strategy->has_active_plan()) {
                accepted = true;
                break;
            }
        }

        if (!accepted && plans.size() > 1) {
            LOG_DEBUG("[StrategyEngine] All {} plans rejected for {} — no fallback available",
                      plans.size(), ticker);
        }

        if (plans.size() > 1) {
            LOG_TRACE("[StrategyEngine] {} plans for {} — selected priority {} {}others",
                      plans.size(), ticker, plans.front().priority,
                      accepted ? "accepted, dropped " + std::to_string(plans.size() - 1) : "all rejected");
        }
    }
}

void StrategyEngine::reset_strategy_plan(const Ticker& ticker, const std::string& strategy_name) {
    auto it = ticker_strategies_.find(ticker);
    if (it != ticker_strategies_.end()) {
        for (auto& strat : it->second) {
            if (strat->name() == strategy_name) {
                strat->reset_active_plan();
            }
        }
    }
}

void StrategyEngine::set_on_plan(PlanCallback cb) {
    on_plan_ = std::move(cb);
}

void StrategyEngine::notify_plan_accepted(const TradePlan& plan) {
    auto it = ticker_strategies_.find(plan.ticker);
    if (it != ticker_strategies_.end()) {
        for (auto& strat : it->second) {
            if (strat->name() == plan.strategy_name) {
                strat->on_plan_accepted(plan);
                LOG_INFO("[StrategyEngine] {}: {} notified of plan acceptance",
                         plan.ticker, plan.strategy_name);
            }
        }
    }
}

void StrategyEngine::set_close_callback(CloseCallback cb) {
    close_cb_ = std::move(cb);
}

std::vector<StrategyState> StrategyEngine::get_all_states() const {
    std::vector<StrategyState> result;
    
    auto collect = [&](const auto& list) {
        for (const auto& strat : list) {
            if (strat) {
                result.push_back(strat->get_state());
            }
        }
    };

    for (const auto& [ticker, list] : ticker_strategies_) {
        collect(list);
    }
    collect(global_strategies_);

    return result;
}

MarketRegime StrategyEngine::classify_regime(const FeatureFrame& frame) const {
    // STRATEGIES.md § 6: 3-state regime classifier.
    // Full HMM implementation is Phase 5; this is a threshold-based fallback.
    
    // News regime: very high volatility
    if (frame.volatility_1min_bps > 50.0) {
        return MarketRegime::News;
    }

    // Trend regime: strong directional movement with high correlation
    const double trend_slope = frame.price_change_30s;  // % over 30s
    if (std::abs(trend_slope) > 0.3 &&                    // >0.3% directional move
        frame.leader_correlation > 0.6) {                 // correlated with leader
        return MarketRegime::Trend;
    }

    // Range regime: everything else (low vol, no strong trend)
    return MarketRegime::Range;
}

MarketRegime StrategyEngine::current_regime(const Ticker& ticker) const {
    auto it = regimes_.find(ticker);
    if (it == regimes_.end()) return MarketRegime::Unknown;
    return it->second;
}

} // namespace trade_bot
