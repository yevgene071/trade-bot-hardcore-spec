#pragma once

#include "FeatureFrame.hpp"

#include <vector>
#include <cstddef>

namespace trade_bot {

/**
 * Lightweight snapshot of key features for dashboard chart rendering.
 * Kept compact to fit 300 points in ring buffer without excessive memory.
 */
struct ChartPoint {
    int64_t ts_unix_ms{0};
    double  mid{0.0};
    double  best_bid{0.0};
    double  best_ask{0.0};
    double  spread_bps{0.0};
    double  buy_vol_5s{0.0};
    double  sell_vol_5s{0.0};
    double  volatility_1min_bps{0.0};
    double  tape_aggression{0.0};
    double  leader_change_1s{0.0};
    double  leader_correlation{0.0};
};

/**
 * Ring buffer for dashboard chart history (~5 min at 1s cadence).
 *
 * Thread-safety: intended for single-producer single-consumer usage
 * where push() is called from the trading loop (under TickerController mutex)
 * and snapshot() is called from the dashboard strand.
 */
class ChartHistory {
public:
    static constexpr std::size_t kCapacity = 600; // 60 s at 10 Hz (pushed every tick from TickerController::tick)

    ChartHistory() {
        buffer_.reserve(kCapacity);
    }

    /// Push a new point from a FeatureFrame. O(1) amortized.
    void push(const FeatureFrame& frame) {
        ChartPoint pt;
        auto ts = std::chrono::duration_cast<std::chrono::milliseconds>(
            frame.timestamp.time_since_epoch()).count();
        pt.ts_unix_ms          = static_cast<int64_t>(ts);
        pt.mid                 = frame.mid;
        pt.best_bid            = frame.best_bid;
        pt.best_ask            = frame.best_ask;
        pt.spread_bps          = frame.spread_bps;
        pt.buy_vol_5s          = frame.buy_vol_5s;
        pt.sell_vol_5s         = frame.sell_vol_5s;
        pt.volatility_1min_bps = frame.volatility_1min_bps;
        pt.tape_aggression     = frame.tape_aggression;
        pt.leader_change_1s    = frame.leader_change_1s;
        pt.leader_correlation  = frame.leader_correlation;

        if (buffer_.size() < kCapacity) {
            buffer_.push_back(pt);
        } else {
            buffer_[write_pos_] = pt;
            write_pos_ = (write_pos_ + 1) % kCapacity;
        }
    }

    /// Return a copy of the buffer in chronological order (oldest first).
    [[nodiscard]] std::vector<ChartPoint> snapshot() const {
        if (buffer_.empty()) return {};

        if (buffer_.size() < kCapacity) {
            return buffer_; // not yet wrapped
        }

        // Wrapped: reconstruct chronological order
        std::vector<ChartPoint> out;
        out.reserve(kCapacity);
        for (std::size_t i = 0; i < kCapacity; ++i) {
            out.push_back(buffer_[(write_pos_ + i) % kCapacity]);
        }
        return out;
    }

    [[nodiscard]] std::size_t size() const noexcept { return buffer_.size(); }

private:
    std::vector<ChartPoint> buffer_;
    std::size_t             write_pos_{0};
};

} // namespace trade_bot
