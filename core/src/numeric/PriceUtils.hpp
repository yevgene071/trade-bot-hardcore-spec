#pragma once

#include <cmath>

namespace trade_bot {

/**
 * Rounds a value to the nearest price tick.
 * Ensures that price is valid for the exchange.
 */
inline double round_to_tick(double val, double tick) {
    if (tick <= 0) return val;
    return std::round(val / tick) * tick;
}

inline double floor_to_tick(double val, double tick) {
    if (tick <= 0) return val;
    // Add epsilon to avoid precision issues (e.g. 1.0 / 0.1 resulting in 9.999...)
    return std::floor((val + tick * 1e-9) / tick) * tick;
}

inline double ceil_to_tick(double val, double tick) {
    if (tick <= 0) return val;
    return std::ceil((val - tick * 1e-9) / tick) * tick;
}

} // namespace trade_bot
