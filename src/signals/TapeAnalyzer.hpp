#pragma once

#include "IDetector.hpp"
#include "SignalBus.hpp"
#include "marketdata/OrderBook.hpp"
#include "marketdata/TradeStream.hpp"
#include "numeric/Cusum.hpp"
#include "numeric/Ema.hpp"

#include <chrono>

namespace trade_bot {

/**
 * T2-TAPE: Analyzes trade stream for bursts, fades, and flushes.
 */
class TapeAnalyzer : public IDetector {
public:
    struct Config {
        // Burst (Hawkes)
        double burst_ratio{3.0};
        double burst_total_intensity_k{3.0}; // k * mu
        
        // Fade (CUSUM)
        double fade_ratio{0.2};
        double fade_cusum_h{5.0};
        
        // Flush (T-Digest)
        double flush_min_size_usd{200000.0};
        double flush_min_move_bps{15.0};
        
        // Distribution
        double distribution_max_range_bps{20.0};
        double distribution_min_volume_usd{500000.0};
    };

    TapeAnalyzer(Ticker ticker,
                SignalBus& bus,
                const OrderBook& book,
                const TradeStream& stream,
                Config cfg);

    TapeAnalyzer(Ticker ticker,
                SignalBus& bus,
                const OrderBook& book,
                const TradeStream& stream);

    void on_frame(const FeatureFrame& frame) override;
    void on_trade(const Trade& trade) override;
    void on_book_update(const OrderBookUpdate& update) override;

private:
    Ticker          ticker_;
    SignalBus&      bus_;
    const OrderBook& book_;
    const TradeStream& stream_;
    Config          cfg_;

    Ema<double>     background_intensity_; // mu
    double          peak_intensity_60s_{0.0};
    std::chrono::system_clock::time_point last_peak_ts_;
    
    Cusum<double>   fade_cusum_;
    
    bool burst_signal_active_{false};
    bool fade_signal_active_{false};
    bool distribution_signal_active_{false};
};

} // namespace trade_bot
