#pragma once

#include <cmath>

namespace trade_bot {

/**
 * Kahan summation algorithm to reduce floating point errors in sums.
 * Useful for PnL calculation.
 */
template <typename T>
class KahanAccumulator {
public:
    void add(T x) {
        T y = x - correction_;
        T t = sum_ + y;
        correction_ = (t - sum_) - y;
        sum_ = t;
    }

    T sum() const { return sum_; }

private:
    T sum_ = 0;
    T correction_ = 0;
};

} // namespace trade_bot
