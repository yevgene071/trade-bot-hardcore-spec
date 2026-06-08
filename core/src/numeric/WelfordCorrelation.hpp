#pragma once

#include "Welford.hpp"
#include <cmath>

namespace trade_bot {

/**
 * Online Pearson correlation using extended Welford algorithm.
 * Based on Schubert & Gertz (2018).
 */
template <typename T>
class WelfordCorrelation {
public:
    void update(T x, T y) {
        n_++;
        T dy = y - acc_y_.mean(); // dy using old mean of Y
        acc_x_.update(x);
        acc_y_.update(y);
        
        // n * Cov(X,Y) accumulator
        c_xy_ += (x - acc_x_.mean()) * dy;
    }

    size_t count() const { return n_; }
    T correlation() const {
        if (n_ < 2) return T(0);
        T var_x = acc_x_.variance() * static_cast<T>(n_ - 1); // M2_x
        T var_y = acc_y_.variance() * static_cast<T>(n_ - 1); // M2_y
        if (var_x == T(0) || var_y == T(0)) return T(0);
        return c_xy_ / std::sqrt(var_x * var_y);
    }

private:
    size_t n_ = 0;
    WelfordAccumulator<T> acc_x_;
    WelfordAccumulator<T> acc_y_;
    T c_xy_ = 0;
};

} // namespace trade_bot
