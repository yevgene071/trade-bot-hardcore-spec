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
    T variance() const {
        if (n_ < 2) return T(0);
        // T4-MATH: Ensure variance is never negative due to precision jitter (#143)
        // Bessel's correction: divide by (n-1) for unbiased sample variance.
        return std::max(T(0), m2_ / static_cast<T>(n_ - 1));
    }
    T stdev() const { 
        T v = variance();
        return (v > 0) ? std::sqrt(v) : T(0); 
    }

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
        if (weight <= 0 || !std::isfinite(x)) return;
        T old_weight_sum = weight_sum_;
        weight_sum_ += weight;
        T delta = x - mean_;
        T r = delta * weight / weight_sum_;
        mean_ += r;
        m2_ += old_weight_sum * delta * r;
        // L2: Halving m2_ and weight_sum_ preserves variance() but biases
        // subsequent mean_ updates (denominator becomes old/2 + new instead of
        // old + new). Removed: overflow at 1e18 is theoretical for double weights
        // and the "fix" introduced worse errors than it prevented.
    }

    T weight_sum() const { return weight_sum_; }
    T mean() const { return mean_; }
    T variance() const { 
        if (weight_sum_ <= 0) return T(0);
        return std::max(T(0), m2_ / weight_sum_); 
    }
    T stdev() const { 
        T v = variance();
        return (v > 0) ? std::sqrt(v) : T(0); 
    }

private:
    T weight_sum_ = 0;
    T mean_ = 0;
    T m2_ = 0;
};

} // namespace trade_bot
