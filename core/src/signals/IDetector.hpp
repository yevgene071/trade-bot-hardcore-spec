#pragma once

#include "domain/Types.hpp"
#include "features/FeatureFrame.hpp"

namespace trade_bot {

/**
 * Base interface for all signal detectors.
 */
class IDetector {
public:
    virtual ~IDetector() = default;

    /// Process a new feature frame (10-20 Hz).
    virtual void on_frame(const FeatureFrame& frame) = 0;

    /// Process a new trade (event-driven).
    virtual void on_trade(const Trade& trade) = 0;

    /// Process a book update (event-driven).
    virtual void on_book_update(const OrderBookUpdate& update) = 0;

    /// Return performance stage name for latency tracking.
    virtual const char* perf_stage_name() const noexcept = 0;
};

} // namespace trade_bot
