#include "TapeAnalyzer.hpp"
#include "perf/TraceContext.hpp"
#include <cmath>

namespace trade_bot {

TapeAnalyzer::TapeAnalyzer(Ticker ticker,
                          SignalBus& bus,
                          const OrderBook& book,
                          const TradeStream& stream,
                          const TickerUniverse& universe,
                          Config cfg)
    : ticker_(std::move(ticker))
    , bus_(bus)
    , book_(book)
    , stream_(stream)
    , universe_(universe)
    , cfg_(cfg)
    , background_intensity_(Ema<double>::from_period(cfg.background_intensity_period)) // Z2
{}

TapeAnalyzer::TapeAnalyzer(Ticker ticker,
                          SignalBus& bus,
                          const OrderBook& book,
                          const TradeStream& stream,
                          const TickerUniverse& universe)
    : TapeAnalyzer(std::move(ticker), bus, book, stream, universe, Config{}) {}

void TapeAnalyzer::on_frame(const FeatureFrame& frame) {
    if (frame.ticker != ticker_) return;
    if (!frame.valid) return;

    auto stats = stream_.get_stats();
    double current_rate = stats.hawkes_intensity_total;
    
    // Update mu (background intensity)
    background_intensity_.update(current_rate);
    double mu = background_intensity_.value();

    // Z4: keep last_pre_trade_mid_ valid even before the first book update arrives
    if (frame.mid > 0.0 && last_pre_trade_mid_ == 0.0) {
        last_pre_trade_mid_ = frame.mid;
    }

    // Z1: use a proper sliding window for peak intensity over the last 60 seconds.
    intensity_history_.push_back({frame.timestamp, current_rate});
    
    // Evict old entries
    while (intensity_history_.size() > 0 && 
           (frame.timestamp - intensity_history_.front().ts) > std::chrono::seconds(60)) {
        intensity_history_.pop_front();
    }
    
    // Recalculate peak
    peak_intensity_60s_ = 0.0;
    for (size_t i = 0; i < intensity_history_.size(); ++i) {
        if (intensity_history_[i].rate > peak_intensity_60s_) {
            peak_intensity_60s_ = intensity_history_[i].rate;
        }
    }

    // 1. TapeBurst Detection
    bool burst_now = false;
    Side burst_side = Side::None;
    double ratio = 0.0;

    if (stats.hawkes_intensity_buy >= stats.hawkes_intensity_sell * cfg_.burst_ratio) {
        burst_now = true;
        burst_side = Side::Buy;
        ratio = stats.hawkes_intensity_buy / std::max(cfg_.burst_min_denominator, stats.hawkes_intensity_sell); // Z3
    } else if (stats.hawkes_intensity_sell >= stats.hawkes_intensity_buy * cfg_.burst_ratio) {
        burst_now = true;
        burst_side = Side::Sell;
        ratio = stats.hawkes_intensity_sell / std::max(cfg_.burst_min_denominator, stats.hawkes_intensity_buy); // Z3
    }

    if (burst_now && current_rate >= cfg_.burst_total_intensity_k * mu) {
        if (!burst_signal_active_) {
            Signal s {
                .kind = SignalKind::TapeBurst,
                .timestamp = frame.timestamp,
                .ticker = ticker_,
                .price = frame.mid,
                .confidence = std::min(1.0, ratio / 10.0),
                .payload = {
                    .side = burst_side == Side::Buy ? "Buy" : "Sell",
                    .ratio = ratio,
                    .intensity = current_rate
                }
            };
            s.trigger_trace_id = current_trace_context().trace_id;
            bus_.publish(s);
            burst_signal_active_ = true;
        }
    } else {
        burst_signal_active_ = false;
    }

    // 2. TapeFade Detection (CUSUM)
    // target = peak * fade_ratio, drift = 0.05 * peak
    double target = peak_intensity_60s_ * cfg_.fade_ratio;
    double drift = 0.05 * peak_intensity_60s_;
    
    fade_cusum_.update(current_rate, target, drift);

    if (fade_cusum_.value() >= cfg_.fade_cusum_h && peak_intensity_60s_ > cfg_.fade_peak_gate) { // Z5
        if (!fade_signal_active_) {
            Signal s {
                .kind = SignalKind::TapeFade,
                .timestamp = frame.timestamp,
                .ticker = ticker_,
                .price = frame.mid,
                .confidence = 1.0,
                .payload = {
                    .peak_rate = peak_intensity_60s_,
                    .current_rate = current_rate,
                    .cusum = fade_cusum_.value()
                }
            };
            s.trigger_trace_id = current_trace_context().trace_id;
            bus_.publish(s);
            fade_signal_active_ = true;
        }
    } else if (current_rate > target * cfg_.fade_reset_ratio) { // Z5
        fade_signal_active_ = false;
        fade_cusum_.reset();
    }

    // 4. TapeDistribution Detection (consolidation / accumulation).
    // Spec — fixes #118.
    // A consolidation regime is characterised by:
    //   - low intra-window price volatility (stdev of log returns), AND
    //   - non-trivial volume that is NOT being absorbed into a directional move.
    // The HMM market-regime classifier (T2-APPROACH) consumes this to switch
    // between breakout and bounce strategies.
    //
    // Volatility is read from the FeatureFrame in basis points (already
    // computed as Welford stdev of 1-min log returns by FeatureExtractor).
    // Volume is converted from base-asset units to USD via the current mid.
    const double total_vol_30s = stats.buy_vol_30s + stats.sell_vol_30s;
    const double volume_usd_30s = total_vol_30s * frame.mid;
    const bool low_dispersion = frame.volatility_1min_bps > 0.0 &&
                                frame.volatility_1min_bps <= cfg_.distribution_max_range_bps;
    const bool sustained_volume = volume_usd_30s >= universe_.calibrated_min_size_usd(
        ticker_, cfg_.distribution_min_volume_usd);

    if (low_dispersion && sustained_volume) {
        if (!distribution_signal_active_) {
            Signal s {
                .kind = SignalKind::TapeDistribution,
                .timestamp = frame.timestamp,
                .ticker = ticker_,
                .price = frame.mid,
                .confidence = 1.0,
                .payload = {
                    .volatility_bps = frame.volatility_1min_bps,
                    .volume_usd_30s = volume_usd_30s,
                    .max_range_bps = cfg_.distribution_max_range_bps
                }
            };
            s.trigger_trace_id = current_trace_context().trace_id;
            bus_.publish(s);
            distribution_signal_active_ = true;
        }
    } else {
        distribution_signal_active_ = false;
    }
}

void TapeAnalyzer::on_trade(const Trade& trade) {
    auto stats = stream_.get_stats();
    
    // 3. TapeFlush Detection (Outlier via T-Digest)
    double size_usd = trade.size * trade.price;
    
    // Use q99 from T-Digest as adaptive threshold
    double adaptive_threshold = stats.q99_size * trade.price;
    double min_threshold = std::max(
        universe_.calibrated_min_size_usd(ticker_, cfg_.flush_min_size_usd),
        adaptive_threshold);

    if (size_usd >= min_threshold) {
        const double mid_val = last_pre_trade_mid_;
        if (mid_val > 0.0) {
            double delta_bps = std::abs(trade.price - mid_val) / mid_val * 10000.0;
            bool cooldown_ok = last_flush_ts_ == std::chrono::system_clock::time_point{} ||
                               (trade.timestamp - last_flush_ts_) >= cfg_.flush_cooldown;
            if (delta_bps >= cfg_.flush_min_move_bps && cooldown_ok) {
                last_flush_ts_ = trade.timestamp;
                Signal s {
                    .kind = SignalKind::TapeFlush,
                    .timestamp = trade.timestamp,
                    .ticker = ticker_,
                    .price = trade.price,
                    .confidence = 1.0,
                    .payload = {
                        .size_usd = size_usd,
                        .delta_bps = delta_bps
                    }
                };
                s.trigger_trace_id = current_trace_context().trace_id;
                bus_.publish(s);
            }
        }
    }
}

void TapeAnalyzer::on_book_update(const OrderBookUpdate& /*update*/) {
    if (auto m = book_.mid()) last_pre_trade_mid_ = *m;
}

} // namespace trade_bot
