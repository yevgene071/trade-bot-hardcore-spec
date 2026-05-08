#pragma once

#include "FeatureFrame.hpp"
#include "marketdata/LeaderTracker.hpp"
#include "marketdata/OrderBook.hpp"
#include "marketdata/TradeStream.hpp"
#include "numeric/HdrHistogramWrapper.hpp"

#include <chrono>
#include <deque>

namespace trade_bot {

/**
 * T1-FRAME: assembles a FeatureFrame from the upstream stateful sources
 * (OrderBook, TradeStream, optional LeaderTracker) at the configured cadence
 * (typically 10 Hz). Hot path is allocation-free after construction:
 * mid_history_ uses a fixed-capacity deque seeded by reserve_history_.
 *
 * Latency is captured in two HdrHistograms exposed for diagnostics:
 *   - book_to_feature_us — time spent reading order-book and depth
 *   - feature_total_us   — end-to-end extract() latency
 */
class FeatureExtractor {
public:
    struct Config {
        std::size_t reserve_history{600};   // 60 s @ 10 Hz
        int64_t     latency_max_us{1'000'000};
        int         hdr_significant_digits{3};
    };

    explicit FeatureExtractor(Ticker ticker);
    FeatureExtractor(Ticker ticker, Config cfg);

    /// Wire the upstream sources. Lifetime of these objects must exceed the
    /// FeatureExtractor's own.
    void set_sources(const OrderBook*    ob,
                     const TradeStream*  ts,
                     const LeaderTracker* lt = nullptr);

    /// Build the frame for `now`. Uses the most recent state of each source.
    FeatureFrame extract(std::chrono::system_clock::time_point now);

    const HdrHistogram& book_to_feature_hist() const noexcept { return hist_book_; }
    const HdrHistogram& feature_total_hist()   const noexcept { return hist_total_; }

    std::size_t history_size() const noexcept { return mid_count_; }

private:
    struct MidSample {
        std::chrono::system_clock::time_point t;
        double mid;
    };

    Ticker                  ticker_;
    Config                  cfg_;
    const OrderBook*        ob_{nullptr};
    const TradeStream*      ts_{nullptr};
    const LeaderTracker*    lt_{nullptr};

    std::vector<MidSample>  mid_history_;
    std::size_t             mid_head_{0};
    std::size_t             mid_count_{0};
    HdrHistogram            hist_book_;
    HdrHistogram            hist_total_;
};

}  // namespace trade_bot
