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
        return std::max(T(0), m2_ / n_); 
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
        
        // T4-MATH: Protect against runaway error accumulation
        if (weight_sum_ > 1e18) { // Normalize if sum becomes too large
            m2_ /= 2.0;
            weight_sum_ /= 2.0;
        }
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
