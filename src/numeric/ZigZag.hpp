#pragma once

#include "domain/Types.hpp"
#include <vector>
#include <cmath>

namespace trade_bot {

/**
 * ZigZag algorithm for pullback detection.
 */
class ZigZag {
public:
    enum class State { FindingHigh, FindingLow };

    struct Peak {
        double price;
        size_t index;
        bool   is_high;
    };

    static std::vector<Peak> calculate(const std::vector<double>& prices, double min_swing_bps) {
        if (prices.size() < 2) return {};

        std::vector<Peak> peaks;
        double first = prices[0];
        double last_peak_price = first;
        size_t last_peak_idx = 0;
        State  state = State::FindingHigh; // Start state depends on first movement

        for (size_t i = 1; i < prices.size(); ++i) {
            double cur = prices[i];
            double diff_bps = std::abs(cur - last_peak_price) / last_peak_price * kBpsBase;

            if (state == State::FindingHigh) {
                if (cur > last_peak_price) {
                    last_peak_price = cur;
                    last_peak_idx = i;
                } else if (diff_bps >= min_swing_bps) {
                    peaks.push_back({last_peak_price, last_peak_idx, true});
                    last_peak_price = cur;
                    last_peak_idx = i;
                    state = State::FindingLow;
                }
            } else {
                if (cur < last_peak_price) {
                    last_peak_price = cur;
                    last_peak_idx = i;
                } else if (diff_bps >= min_swing_bps) {
                    peaks.push_back({last_peak_price, last_peak_idx, false});
                    last_peak_price = cur;
                    last_peak_idx = i;
                    state = State::FindingHigh;
                }
            }
        }
        
        return peaks;
    }
};

} // namespace trade_bot
