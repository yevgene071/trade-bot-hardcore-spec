#pragma once

#include "domain/Types.hpp"
#include "features/FeatureFrame.hpp"
#include "signals/Signal.hpp"
#include "TradePlan.hpp"
#include "transport/IClock.hpp"

#include <optional>
#include <string>
#include <vector>

namespace trade_bot {

/**
 * Lifecycle state of a strategy for dashboard visibility.
 */
enum class StrategyReadyState {
    Cold,       // just created, no data yet
    Warming,    // accumulating data, not all conditions met
    Ready,      // all conditions met, waiting for signal
    Planning,   // TradePlan generated, awaiting executor
    Trading,    // in active trade
    Cooldown    // pause after exit
};

/**
 * A single pre-flight condition that the strategy checks.
 */
struct StrategyCondition {
    std::string name;      // e.g. "CUSUM warmup", "Leader corr"
    double      current{}; // current value
    double      target{};  // threshold
    bool        met{false}; // is condition satisfied?
    std::string unit;      // "%", "bps", "samples"
};

/**
 * Dashboard-facing snapshot of a strategy's readiness and conditions.
 */
struct StrategyState {
    Ticker                            ticker;
    std::string                       strategy_name;
    StrategyReadyState                ready_state{StrategyReadyState::Cold};
    double                            readiness_pct{0.0}; // met/total * 100
    std::vector<StrategyCondition>    conditions;
    std::string                       last_reject_reason;
    double                            seconds_since_last_reject{0.0};
    int                               signals_last_60s{0};
};

/**
 * Base interface for all trading strategies.
 * 
 * P0 DETERMINISM: Strategies must use the injected IClock instead of
 * std::chrono::system_clock::now() to enable deterministic replay.
 */
class IStrategy {
public:
    virtual ~IStrategy() = default;

    virtual const std::string& name() const = 0;

    /// The ticker this strategy is focused on.
    virtual const Ticker& ticker() const = 0;

    /// Process periodic feature update.
    virtual void on_frame(const FeatureFrame& frame) = 0;

    /// Process a signal from any detector. Can return a TradePlan immediately
    /// for event-driven strategies (e.g. BounceFromDensity reacting to LevelApproach).
    /// Strategies that require waiting (e.g. LeaderLag with delay) return nullopt
    /// and generate plans in tick() instead.
    /// @param signal The signal to process
    /// @param now Current time from injected clock (deterministic in replay mode)
    virtual std::optional<TradePlan> on_signal(const Signal& signal, std::chrono::system_clock::time_point now) = 0;

    /// Core logic tick. Returns a TradePlan if entry conditions are met.
    /// Used for TTL-based logic (timeouts, waiting periods) and strategies
    /// that don't react to specific signals.
    /// @param now Current time from injected clock (deterministic in replay mode)
    virtual std::optional<TradePlan> tick(std::chrono::system_clock::time_point now) = 0;

    // P0 DETERMINISM: Clock is now injected via constructor (removed set_clock())

    /// Returns true if this strategy currently has an active (pending) plan.
    /// Used by StrategyEngine to determine if a fallback plan should be tried.
    virtual bool has_active_plan() const { return false; }

    /// Resets the current active/pending plan (e.g. if rejected by risk).
    virtual void reset_active_plan() {}

    /// Called by StrategyEngine when a TradePlan from this strategy has been
    /// accepted by the executor. Strategy should begin monitoring for post-entry
    /// invalidation conditions (STRATEGIES.md § 0.4, § 3.7).
    virtual void on_plan_accepted(const TradePlan& /*plan*/) {}

    /// Check if the active trade should be force-closed based on the latest
    /// feature frame. Returns a reason string, or nullopt if trade should stay open.
    /// Only meaningful after on_plan_accepted() was called.
    virtual std::optional<FixedString<32>> check_close_conditions(const FeatureFrame& /*latest_frame*/) {
        return std::nullopt;
    }

    /// Returns the current readiness state of the strategy for dashboard display.
    /// All conditions have equal weight: readiness_pct = met/total * 100.
    /// For Planning/Trading/Cooldown states, readiness_pct = 100.
    virtual StrategyState get_state() const = 0;

    /// Priority for conflict resolution (STRATEGIES.md § 7).
    /// Lower number = higher priority. Default: 100 (lowest).
    virtual int priority() const { return 100; }
};

} // namespace trade_bot
