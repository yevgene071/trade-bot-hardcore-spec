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

/**
 * Weighted Welford's algorithm for online calculation of mean and variance.
 */
template <typename T>
class WeightedWelfordAccumulator {
public:
    void update(T x, T weight) {
        if (weight <= 0) return;
        T old_weight_sum = weight_sum_;
        weight_sum_ += weight;
        T delta = x - mean_;
        T r = delta * weight / weight_sum_;
        mean_ += r;
        m2_ += old_weight_sum * delta * r;
    }

    T weight_sum() const { return weight_sum_; }
    T mean() const { return mean_; }
    T variance() const { return (weight_sum_ > 0) ? m2_ / weight_sum_ : T(0); }
    T stdev() const { return std::sqrt(variance()); }

private:
    T weight_sum_ = 0;
    T mean_ = 0;
    T m2_ = 0;
};

} // namespace trade_bot
