#pragma once

#include "Ema.hpp"

namespace trade_bot {

/**
 * Double Exponential Moving Average.
 * DEMA(x) = 2 * EMA(x) - EMA(EMA(x))
 */
template <typename T>
class Dema {
public:
    explicit Dema(T alpha) : ema1_(alpha), ema2_(alpha) {}

    static Dema from_period(int n) {
        return Dema(T(2) / (T(n) + T(1)));
    }

    void update(T x) {
        ema1_.update(x);
        ema2_.update(ema1_.value());
    }

    T value() const {
        return T(2) * ema1_.value() - ema2_.value();
    }

private:
    Ema<T> ema1_;
    Ema<T> ema2_;
};

} // namespace trade_bot
