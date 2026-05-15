#pragma once

#include "domain/Types.hpp"
#include "numeric/HdrHistogramWrapper.hpp"
#include "transport/MarketDataFeed.hpp"

#include <chrono>
#include <cstdint>
#include <mutex>
#include <unordered_map>

namespace trade_bot {

/**
 * T0-MONITOR-TIMESTAMPS: monitors latency and jitter between trade_update and
 * orderbook_update events received from MetaScalp.
 *
 * Threshold violations are reported via WARN logs. Aggregate metrics:
 *   - trade_orderbook_latency_us: p99 latency between consecutive trade and
 *     orderbook events (recorded into HdrHistogram)
 *   - trade_orderbook_jitter_us:  hdr stddev of the same distribution
 *
 * Pairing rule: every incoming event records the absolute time delta to the
 * most recent counterpart event of the *same ticker* (if seen within
 * `pair_window_us`). This produces one latency sample per cross-stream
 * transition.
 */
class TimestampMonitor : public IMarketDataListener {
public:
    struct Config {
        int64_t highest_latency_us = 1'000'000;  // 1 s upper bound for HdrHistogram
        int sig_digits = 3;
        int64_t pair_window_us = 5'000'000;      // 5 s — events farther apart are not paired
        int64_t warn_p99_us = 10'000;            // spec: p99 < 10 ms
        int64_t warn_jitter_us = 2'000;          // spec: jitter < 2 ms
        int64_t warn_check_every_n = 1000;       // emit threshold check after N samples
    };

    TimestampMonitor();
    explicit TimestampMonitor(Config cfg);

    // IMarketDataListener
    void on_trade(const Ticker& ticker, const Trade& trade) override;
    void on_trades(const Ticker& ticker, const std::vector<Trade>& trades) override;
    void on_orderbook_snapshot(const OrderBookSnapshot& snapshot) override;
    void on_orderbook_update(const OrderBookUpdate& update) override;
    void on_order_update(const OrderUpdate& /*u*/) override {}
    void on_position_update(const PositionUpdate& /*u*/) override {}
    void on_balance_update(const BalanceUpdate& /*u*/) override {}
    void on_finres_update(const FinresUpdate& /*u*/) override {}
    void on_error(const std::string& /*msg*/) override {}

    // Metrics access
    int64_t latency_p99_us() const;
    int64_t latency_p50_us() const;
    double  jitter_us() const;     // = stddev of latency distribution
    int64_t sample_count() const;
    void    reset();

private:
    using clock = std::chrono::steady_clock;

    void record_pair(const Ticker& ticker, clock::time_point now, bool from_trade);
    void check_thresholds_locked();

    Config cfg_;

    struct TickerState {
        clock::time_point last_trade{};
        clock::time_point last_book{};
        bool has_trade{false};
        bool has_book{false};
    };

    mutable std::mutex mtx_;
    HdrHistogram latency_;
    std::unordered_map<Ticker, TickerState> per_ticker_;
    int64_t samples_since_check_{0};
};

} // namespace trade_bot
