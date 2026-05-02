#pragma once

#include "strategy/TradePlan.hpp"

namespace trade_bot {

/**
 * Interface for executing trade plans.
 */
class IExecutor {
public:
    virtual ~IExecutor() = default;

    /// Submit a new trade plan for execution.
    virtual void submit(const TradePlan& plan) = 0;

    /// Cancel all active orders for a ticker.
    virtual void cancel_all(const Ticker& ticker) = 0;
};

} // namespace trade_bot
