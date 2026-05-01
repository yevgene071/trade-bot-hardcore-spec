#pragma once

#include <cstddef>
#include <cmath>

namespace trade_bot {

/**
 * Welford's algorithm for online calculation of mean and variance.
 * Numerically stable.
 */
template <typename T>
class WelfordAccumulator {
public:
    void update(T x) {
        n_++;
        T delta = x - mean_;
        mean_ += delta / n_;
        T delta2 = x - mean_;
        m2_ += delta * delta2;
    }

    size_t count() const { return n_; }
    T mean() const { return mean_; }
    T variance() const { return (n_ > 1) ? m2_ / n_ : T(0); }
    T stdev() const { return std::sqrt(variance()); }

private:
    size_t n_ = 0;
    T mean_ = 0;
    T m2_ = 0;
};

} // namespace trade_bot
