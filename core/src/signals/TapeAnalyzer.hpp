#pragma once

#include "IDetector.hpp"
#include "SignalBus.hpp"
#include "marketdata/OrderBook.hpp"
#include "marketdata/TradeStream.hpp"
#include "numeric/Cusum.hpp"
#include "numeric/Ema.hpp"
#include "universe/TickerUniverse.hpp"

#include "utils/CircularBuffer.hpp"
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
        double burst_min_denominator{0.1};   // Z3: floor for buy/sell ratio denominator

        // Background intensity EMA
        int    background_intensity_period{300}; // Z2: ~30s at 10Hz

        // Peak gate: minimum peak to arm TapeFade; reset ratio to re-arm CUSUM
        double fade_peak_gate{5.0};          // Z5
        double fade_reset_ratio{1.5};        // Z5

        // Fade (CUSUM)
        double fade_ratio{0.2};
        double fade_cusum_h{5.0};

        // Flush (T-Digest)
        double flush_min_size_usd{200000.0};
        double flush_min_move_bps{15.0};
        std::chrono::milliseconds flush_cooldown{1000}; // min gap between TapeFlush signals

        // Distribution
        double distribution_max_range_bps{20.0};
        double distribution_min_volume_usd{500000.0};
    };

    TapeAnalyzer(Ticker ticker,
                SignalBus& bus,
                const OrderBook& book,
                const TradeStream& stream,
                const TickerUniverse& universe,
                Config cfg);

    TapeAnalyzer(Ticker ticker,
                SignalBus& bus,
                const OrderBook& book,
                const TradeStream& stream,
                const TickerUniverse& universe);

    void on_frame(const FeatureFrame& frame) override;
    void on_trade(const Trade& trade) override;
    void on_book_update(const OrderBookUpdate& update) override;
    
    const char* perf_stage_name() const noexcept override { return "tape_eval_us"; }

private:
    Ticker          ticker_;
    SignalBus&      bus_;
    const OrderBook& book_;
    const TradeStream& stream_;
    const TickerUniverse& universe_;
    Config          cfg_;

    Ema<double>     background_intensity_; // mu
    struct IntensitySample {
        std::chrono::system_clock::time_point ts;
        double rate;
    };
    CircularBuffer<IntensitySample, 600> intensity_history_; // 60s at 10Hz
    double          peak_intensity_60s_{0.0};
    
    Cusum<double>   fade_cusum_;
    
    bool burst_signal_active_{false};
    bool fade_signal_active_{false};
    bool distribution_signal_active_{false};
    std::chrono::system_clock::time_point last_flush_ts_;
    double last_pre_trade_mid_{0.0}; // mid captured before each book update, used in on_trade()
};

} // namespace trade_bot
