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
        double hawkes_intensity_buy;
        double hawkes_intensity_sell;
        double hawkes_intensity_total;
    };

    explicit TradeStream(Ticker ticker, 
                        double hawkes_alpha = 1.0, 
                        double hawkes_beta = 0.3466); // half-life 2.0s: beta = ln(2)/2

    void on_trade(const Trade& trade);
    
    /// Update aggregates (evict old trades, recalculate intensity).
    void update(std::chrono::system_clock::time_point now);

    Stats get_stats() const;

    private:
    // Cached T-Digest results for performance
    double cached_q99_ = 0.0;
    size_t last_merge_weight_ = 0;

    void evict_expired_trades_(std::chrono::system_clock::time_point now);

    Ticker ticker_;

    // Trade storage: ring buffer
    std::vector<Trade> trades_;
    size_t head_ = 0;
    size_t count_ = 0;
    static constexpr size_t kMaxTrades = 65536;

    // Window boundaries (indices of the OLDEST trade still in window)
    size_t tail_1s_ = 0;
    size_t tail_5s_ = 0;
    size_t tail_30s_ = 0;

    // Incremental stats
    WelfordAccumulator<double> size_stats_;
    mutable TDigest size_distribution_;

    // Hawkes intensities
    double hawkes_alpha_;
    double hawkes_beta_;
    double hawkes_intensity_buy_ = 0.0;
    double hawkes_intensity_sell_ = 0.0;
    std::chrono::system_clock::time_point last_hawkes_update_;

    // Volume accumulators (maintained incrementally)
    KahanAccumulator<double> buy_vol_1s_;
    KahanAccumulator<double> buy_vol_5s_;
    KahanAccumulator<double> buy_vol_30s_;
    KahanAccumulator<double> sell_vol_1s_;
    KahanAccumulator<double> sell_vol_5s_;
    KahanAccumulator<double> sell_vol_30s_;
    };

} // namespace trade_bot
