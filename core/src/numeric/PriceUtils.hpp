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

} // namespace trade_bot
