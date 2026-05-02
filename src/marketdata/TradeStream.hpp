#pragma once

#include "domain/Types.hpp"
#include "numeric/Welford.hpp"
#include "numeric/Kahan.hpp"
#include "numeric/TDigest.hpp"

#include <vector>
#include <deque>
#include <chrono>

namespace trade_bot {

/**
 * T1-TRADESTREAM: Per-ticker aggregation of recent trades.
 */
class TradeStream {
public:
    struct Stats {
        double prints_per_sec;
        double avg_size;
        double stdev_size;
        double buy_vol_1s;
        double buy_vol_5s;
        double buy_vol_30s;
        double sell_vol_1s;
        double sell_vol_5s;
        double sell_vol_30s;
        double q99_size;      // 99th percentile size from T-Digest
        double hawkes_intensity;
    };

    explicit TradeStream(Ticker ticker, 
                        double hawkes_alpha = 0.5, 
                        double hawkes_beta = 0.5);

    void on_trade(const Trade& trade);
    
    /// Update aggregates (evict old trades, recalculate intensity).
    /// Typically called at 10-20 Hz by FeatureExtractor.
    void update(std::chrono::system_clock::time_point now);

    Stats get_stats() const;

private:
    Ticker ticker_;
    
    // Trade storage: ring buffer (ARCH § 2.3)
    // Using std::vector with manual index for ring buffer or just a deque if small.
    // ARCH says "ring-buffer std::vector фикс. capacity (избегаем std::deque page allocations)"
    std::vector<Trade> trades_;
    size_t head_ = 0;
    size_t count_ = 0;
    static constexpr size_t kMaxTrades = 65536;

    // Incremental algorithms
    WelfordAccumulator<double> size_stats_;
    TDigest size_distribution_;
    
    // Hawkes intensity: λ(t) = μ + Σ α·exp(-β(t-tᵢ))
    double hawkes_alpha_;
    double hawkes_beta_;
    double hawkes_intensity_ = 0.0;
    std::chrono::system_clock::time_point last_hawkes_update_;

    // CUSUM for TapeFade
    double cusum_pos_ = 0.0;
    double cusum_threshold_ = 5.0; // from config

    // Volume accumulators (Kahan summation for precision)
    KahanAccumulator<double> buy_vol_1s_;
    KahanAccumulator<double> buy_vol_5s_;
    KahanAccumulator<double> buy_vol_30s_;
    KahanAccumulator<double> sell_vol_1s_;
    KahanAccumulator<double> sell_vol_5s_;
    KahanAccumulator<double> sell_vol_30s_;

    void recalculate_volumes_(std::chrono::system_clock::time_point now);
};

} // namespace trade_bot
