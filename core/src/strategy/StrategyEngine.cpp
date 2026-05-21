#include "StrategyEngine.hpp"
#include "logger/Logger.hpp"
#include "perf/LatencyTracer.hpp"
#include "perf/PerfRegistry.hpp"

#include <algorithm>

namespace trade_bot {

StrategyEngine::StrategyEngine(SignalBus& signal_bus, std::shared_ptr<IClock> clock)
    : bus_(signal_bus)
    , clock_(std::move(clock)) {

    bus_.subscribe([this](const Signal& s) {
        // I1: use injected clock so replay/tests get deterministic time
        const auto now = clock_ ? clock_->now() : std::chrono::system_clock::now();

        // I2: snapshot under lock to guard concurrent add_strategy/remove_strategy
        absl::btree_map<Ticker, std::vector<IStrategy*>> snap_ticker;
        std::vector<IStrategy*> snap_global;
        {
            std::lock_guard<std::mutex> lk(strategies_mtx_);
            if (!s.ticker.empty()) {
                auto it = ticker_strategies_.find(s.ticker);
                if (it != ticker_strategies_.end()) {
                    for (auto& p : it->second) snap_ticker[s.ticker].push_back(p.get());
                }
            }
            for (auto& p : global_strategies_) snap_global.push_back(p.get());
        }

        for (auto& [ticker, strats] : snap_ticker) {
            for (auto* strat : strats) {
                auto plan = strat->on_signal(s, now);
                if (plan && on_plan_) on_plan_(*plan);
            }
        }
        for (auto* strat : snap_global) {
            auto plan = strat->on_signal(s, now);
            if (plan && on_plan_) on_plan_(*plan);
        }
    });
}

void StrategyEngine::add_strategy(std::unique_ptr<IStrategy> strategy) {
    std::lock_guard<std::mutex> lk(strategies_mtx_);
    if (strategy->ticker().empty()) {
        global_strategies_.push_back(std::move(strategy));
    } else {
        ticker_strategies_[strategy->ticker()].push_back(std::move(strategy));
    }
}

void StrategyEngine::remove_strategy(const Ticker& ticker, const std::string& name) {
    std::lock_guard<std::mutex> lk(strategies_mtx_);
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
            if (it->second.empty()) {
                ticker_strategies_.erase(it);
                // AE2: Clean up regime entry when no more strategies exist for this ticker
                regimes_.erase(ticker);
                plans_scratch_.erase(ticker);
            }
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
    // I4: Clear inner vectors only (preserves capacity), not the outer map.
    for (auto& [ticker, vec] : plans_scratch_) vec.clear();

    auto collect = [&](auto& list) {
        static auto& signal_to_plan_hist = PerfRegistry::instance().get_or_create(kStageSignalToPlan);
        for (auto& strat : list) {
            std::optional<TradePlan> plan;
            {
                LatencyTracer trace(signal_to_plan_hist);
                plan = strat->tick(now);
            }
            if (plan) {
                plans_scratch_[plan->ticker].push_back({strat->priority(), *plan, strat.get()});
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
    for (auto& [ticker, plans] : plans_scratch_) {
        if (plans.empty()) continue;

        // GAP-1 FIX: Suppress plans based on current market regime.
        // News → all entries dangerous (extreme volatility, stops get blown).
        // Trend → BounceFromDensity unreliable (levels get broken in directional moves).
        // Range → LeaderLag lacks momentum signal (leader moves are mean-reverting noise).
        {
            MarketRegime regime = current_regime(ticker);
            if (regime == MarketRegime::News) {
                LOG_DEBUG("[StrategyEngine] {} regime=News — suppressing all plans", ticker);
                for (auto& pw : plans) pw.strategy->reset_active_plan();
                continue;
            }
            // I3: keep predicate side-effect-free; reset strategies after erase
            // by iterating the [new_end, end) tail before erasing it.
            if (regime == MarketRegime::Trend) {
                auto new_end = std::remove_if(plans.begin(), plans.end(),
                    [](const PlanWithPriority& pw) {
                        return pw.plan.strategy_name == "BounceFromDensity";
                    });
                for (auto it = new_end; it != plans.end(); ++it) it->strategy->reset_active_plan();
                plans.erase(new_end, plans.end());
                if (plans.empty()) continue;
            }
            if (regime == MarketRegime::Range) {
                auto new_end = std::remove_if(plans.begin(), plans.end(),
                    [](const PlanWithPriority& pw) {
                        return pw.plan.strategy_name == "LeaderLag";
                    });
                for (auto it = new_end; it != plans.end(); ++it) it->strategy->reset_active_plan();
                plans.erase(new_end, plans.end());
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
            if (accepted) {
                LOG_TRACE("[StrategyEngine] {} plans for {} — priority {} accepted, dropped {}",
                          plans.size(), ticker, plans.front().priority, plans.size() - 1);
            } else {
                LOG_TRACE("[StrategyEngine] {} plans for {} — all rejected", plans.size(), ticker);
            }
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

    // S5-FIX: Increased News threshold from 50→80 bps to reduce false positives
    // on volatile altcoins. Added hysteresis to prevent regime flapping.
    // §7 strategies.common.max_vol_bps = 80: News regime at extreme volatility.
    // RiskManager applies the same threshold globally; this early check avoids
    // unnecessary risk evaluation. Both gates must stay in sync.
    static constexpr double kNewsVolThresholdBps = 80.0;
    static constexpr double kNewsVolHysteresisBps = 60.0;  // Exit News at 60 bps
    
    const auto current = current_regime(frame.ticker);
    if (current == MarketRegime::News && frame.volatility_1min_bps > kNewsVolHysteresisBps) {
        // Stay in News until vol drops below hysteresis threshold
        return MarketRegime::News;
    }
    if (frame.volatility_1min_bps > kNewsVolThresholdBps) {
        return MarketRegime::News;
    }

    // Trend regime: strong directional movement.
    // leader_correlation == 0.0 means no leader data (self-leader or unconfigured) — skip corr check.
    const double trend_slope = frame.price_change_30s;  // % over 30s
    const bool corr_ok = (frame.leader_correlation == 0.0) || (frame.leader_correlation > 0.6);
    if (std::abs(trend_slope) > 0.3 && corr_ok) {
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
