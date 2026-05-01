#pragma once

namespace trade_bot {

/**
 * Exponential Moving Average.
 */
template <typename T>
class Ema {
public:
    explicit Ema(T alpha) : alpha_(alpha) {}

    static Ema from_period(int n) {
        return Ema(T(2) / (T(n) + T(1)));
    }

    void update(T x) {
        if (initialized_) {
            value_ = alpha_ * x + (T(1) - alpha_) * value_;
        } else {
            value_ = x;
            initialized_ = true;
        }
    }

    T value() const { return value_; }

private:
    T alpha_;
    T value_ = 0;
    bool initialized_ = false;
};

} // namespace trade_bot
