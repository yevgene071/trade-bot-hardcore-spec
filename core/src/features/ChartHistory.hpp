#pragma once

#include "FeatureFrame.hpp"

#include <vector>
#include <cstddef>
#include <cstdint>
#include <array>
#include <cmath>
#include <algorithm>

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
    double  leader_lag_ms{0.0};
    double  imbalance{0.0};            // (bid_d10 - ask_d10) / total ∈ [-1, 1]
    double  prints_per_sec{0.0};       // trade stream event rate
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
        pt.leader_lag_ms       = frame.leader_lag_ms;
        pt.imbalance           = frame.imbalance;
        pt.prints_per_sec      = frame.prints_per_sec;

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

    /// Most recent point (O(1)) for live fast-tick streaming.
    [[nodiscard]] std::optional<ChartPoint> latest() const {
        if (buffer_.empty()) return std::nullopt;
        if (buffer_.size() < kCapacity) return buffer_.back();
        return buffer_[(write_pos_ + kCapacity - 1) % kCapacity];
    }

private:
    std::vector<ChartPoint> buffer_;
    std::size_t             write_pos_{0};
};

// ── Liquidity topology history ────────────────────────────────────────────────
// Per-tick compact density profile so the dashboard can render the order-book
// landscape across the WHOLE chart width (historical), not just a live snapshot
// at the right edge. Bins are normalized 0..255 over [lo, hi] to keep the WS
// payload small (kDensityBins bytes/column vs. 40 float price/size pairs).
inline constexpr std::size_t kDensityBins = 24;

struct DensityColumn {
    int64_t ts_unix_ms{0};
    float   lo{0.0f};            // price at bin 0 lower edge
    float   hi{0.0f};            // price at last bin upper edge
    std::array<uint8_t, kDensityBins> bins{}; // normalized density, sqrt-contrast
};

// Build a column from order-book levels (bids ∪ asks). Gaussian-spread each
// level into the bins, then sqrt-normalize to the column max (mirrors the
// frontend density math so live + historical render identically).
template <class LevelVec>
inline DensityColumn build_density_column(int64_t ts_unix_ms,
                                          const LevelVec& bids,
                                          const LevelVec& asks) {
    DensityColumn col;
    col.ts_unix_ms = ts_unix_ms;

    double mn = 0.0, mx = 0.0;
    bool found = false;
    auto scan = [&](const LevelVec& v) {
        for (const auto& l : v) {
            if (l.size <= 0.0 || l.price <= 0.0) continue;
            if (!found) { mn = mx = l.price; found = true; }
            else { mn = std::min(mn, l.price); mx = std::max(mx, l.price); }
        }
    };
    scan(bids);
    scan(asks);
    if (!found) return col; // empty book → zero column

    if (mx - mn < mn * 0.0005) { // degenerate range guard
        const double c = (mx + mn) * 0.5;
        mn = c - c * 0.00025;
        mx = c + c * 0.00025;
    }
    col.lo = static_cast<float>(mn);
    col.hi = static_cast<float>(mx);

    const double sigma = (mx - mn) * 0.02;
    std::array<double, kDensityBins> raw{};
    double maxD = 0.0;
    for (std::size_t i = 0; i < kDensityBins; ++i) {
        const double binP = mn + (i + 0.5) / kDensityBins * (mx - mn);
        double d = 0.0;
        auto acc = [&](const LevelVec& v) {
            for (const auto& l : v) {
                if (l.size <= 0.0) continue;
                const double z = (binP - l.price) / sigma;
                d += l.size * std::exp(-0.5 * z * z);
            }
        };
        acc(bids);
        acc(asks);
        raw[i] = d;
        if (d > maxD) maxD = d;
    }
    if (maxD > 0.0) {
        for (std::size_t i = 0; i < kDensityBins; ++i) {
            const double t = std::sqrt(raw[i] / maxD);
            col.bins[i] = static_cast<uint8_t>(std::lround(t * 255.0));
        }
    }
    return col;
}

// Ring buffer mirroring ChartHistory cadence/capacity (1:1 by tick).
class DensityHistory {
public:
    static constexpr std::size_t kCapacity = ChartHistory::kCapacity;

    DensityHistory() { buffer_.reserve(kCapacity); }

    void push(const DensityColumn& col) {
        if (buffer_.size() < kCapacity) buffer_.push_back(col);
        else { buffer_[write_pos_] = col; write_pos_ = (write_pos_ + 1) % kCapacity; }
    }

    [[nodiscard]] std::vector<DensityColumn> snapshot() const {
        if (buffer_.empty()) return {};
        if (buffer_.size() < kCapacity) return buffer_;
        std::vector<DensityColumn> out;
        out.reserve(kCapacity);
        for (std::size_t i = 0; i < kCapacity; ++i)
            out.push_back(buffer_[(write_pos_ + i) % kCapacity]);
        return out;
    }

private:
    std::vector<DensityColumn> buffer_;
    std::size_t                write_pos_{0};
};

} // namespace trade_bot
