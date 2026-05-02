#pragma once

#include "domain/Types.hpp"
#include "features/FeatureFrame.hpp"
#include "signals/Signal.hpp"
#include "TradePlan.hpp"

#include <optional>
#include <string>

namespace trade_bot {

/**
 * Base interface for all trading strategies.
 */
class IStrategy {
public:
    virtual ~IStrategy() = default;

    virtual const std::string& name() const = 0;

    /// Process periodic feature update.
    virtual void on_frame(const FeatureFrame& frame) = 0;

    /// Process a signal from any detector.
    virtual void on_signal(const Signal& signal) = 0;

    /// Core logic tick. Returns a TradePlan if entry conditions are met.
    virtual std::optional<TradePlan> tick(std::chrono::system_clock::time_point now) = 0;
};

} // namespace trade_bot
