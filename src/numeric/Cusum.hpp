#pragma once

#include <algorithm>

namespace trade_bot {

/**
 * CUSUM (Cumulative Sum) algorithm for change-point detection.
 * Designed to detect a persistent shift in a signal.
 */
template <typename T>
class Cusum {
public:
    void update(T x_n, T target, T drift) {
        // S_n = max(0, S_{n-1} + (target - x_n) - drift)
        // This is for detecting a decrease (target > x_n).
        s_ = std::max(T(0), s_ + (target - x_n) - drift);
    }

    void reset() { s_ = 0; }
    T value() const { return s_; }

private:
    T s_ = 0;
};

} // namespace trade_bot
