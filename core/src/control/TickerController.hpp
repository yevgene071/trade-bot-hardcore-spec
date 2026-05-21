#pragma once

#include "logger/Logger.hpp"
#include "marketdata/OrderBook.hpp"
#include "marketdata/TradeStream.hpp"
#include "features/FeatureExtractor.hpp"
#include "features/ChartHistory.hpp"
#include "signals/DensityDetector.hpp"
#include "signals/IcebergDetector.hpp"
#include "signals/TapeAnalyzer.hpp"
#include "signals/LevelDetector.hpp"
#include "signals/ApproachAnalyzer.hpp"
#include "signals/LeaderSignal.hpp"
#include "transport/MarketDataFeed.hpp"
#include "config/Config.hpp"
#include "perf/LatencyTracer.hpp"
#include "perf/PerfRegistry.hpp"

#include <memory>
#include <mutex>
#include <cmath>

namespace trade_bot {

/**
 * T3-INTEGRATION: Container for all per-ticker market data and signal processing components.
 * Implements IMarketDataListener so it can be registered directly with MarketDataFeed.
 */
struct TickerController : public IMarketDataListener {
    std::unique_ptr<OrderBook> book;
    std::unique_ptr<TradeStream> stream;
    std::unique_ptr<FeatureExtractor> extractor;
    std::unique_ptr<LeaderTracker> leader_tracker;
    
    // Detectors
    std::vector<std::unique_ptr<IDetector>> detectors;
    LeaderSignal* leader_detector{nullptr};
    
    TickerController(Ticker ticker, 
                     SignalBus& bus, 
                     const TickerUniverse& universe,
                     const ClusterSnapshotManager& cluster_mgr,
                     std::optional<Ticker> leader_ticker = std::nullopt) {
        
        auto meta = universe.meta(ticker).value_or(TickerMeta{0.01, 1e-6, 0, 0});

        book = std::make_unique<OrderBook>(ticker, meta.price_increment, meta.size_increment);
        stream = std::make_unique<TradeStream>(ticker);

        if (leader_ticker && *leader_ticker != ticker) {
            leader_tracker = std::make_unique<LeaderTracker>();
        }

        extractor = std::make_unique<FeatureExtractor>(ticker);
        extractor->set_sources(book.get(), stream.get(), leader_tracker.get());

        // Per-ticker BPS scaling: small alts have larger noise/spread → widen BPS thresholds.
        // inv_sqrt range: [1/√5≈0.45 for BTC] to [1/√0.15≈2.58 for small alts].
        const double sf       = universe.volume_scale_factor(ticker);
        const double inv_sqrt = 1.0 / std::sqrt(sf);

        DensityDetector::Config dd_cfg;
        dd_cfg.min_distance_bps = std::min(dd_cfg.min_distance_bps * inv_sqrt, 15.0);
        dd_cfg.max_distance_bps = std::min(dd_cfg.max_distance_bps * inv_sqrt, 300.0);
        detectors.push_back(std::make_unique<DensityDetector>(ticker, bus, *book, universe, dd_cfg));

        detectors.push_back(std::make_unique<IcebergDetector>(ticker, bus, *book, universe));
        detectors.push_back(std::make_unique<TapeAnalyzer>(ticker, bus, *book, *stream, universe));

        LevelDetector::Config ld_cfg;
        ld_cfg.min_reversal_bps      = std::min(ld_cfg.min_reversal_bps      * inv_sqrt, 50.0);
        ld_cfg.cluster_tolerance_bps = std::min(ld_cfg.cluster_tolerance_bps * inv_sqrt, 30.0);
        ld_cfg.approach_trigger_bps  = std::min(ld_cfg.approach_trigger_bps  * inv_sqrt, 30.0);
        auto ld = std::make_unique<LevelDetector>(ticker, bus, *book, cluster_mgr, ld_cfg);
        auto* ld_ptr = ld.get();
        detectors.push_back(std::move(ld));

        ApproachAnalyzer::Config aa_cfg;
        aa_cfg.pullback_min_bps = std::min(aa_cfg.pullback_min_bps * inv_sqrt, 15.0);
        auto aa = std::make_unique<ApproachAnalyzer>(ticker, bus, *book, *ld_ptr, aa_cfg);
        ld_ptr->set_approach_analyzer(aa.get());
        detectors.push_back(std::move(aa));

        if (leader_tracker && leader_ticker) {
            LeaderSignal::Config ls_cfg;
            ls_cfg.min_correlation = Config::get_or<double>("signals.leader.leader_min_correlation", ls_cfg.min_correlation);
            ls_cfg.move_min_pct    = Config::get_or<double>("signals.leader.leader_move_min_pct", ls_cfg.move_min_pct);
            ls_cfg.lag_min_pct     = Config::get_or<double>("signals.leader.leader_lag_min_pct",  ls_cfg.lag_min_pct);
            const auto age_ms      = Config::get_or<int64_t>("signals.leader.lag_max_age_ms",
                                         static_cast<int64_t>(ls_cfg.lag_max_age.count()));
            ls_cfg.lag_max_age     = std::chrono::milliseconds(age_ms);
            auto ls = std::make_unique<LeaderSignal>(ticker, *leader_ticker, bus, *leader_tracker, ls_cfg);
            leader_detector = ls.get();
            detectors.push_back(std::move(ls));
        }
        
        // Cache histogram pointers for each detector (stable for process lifetime)
        det_hist_.reserve(detectors.size());
        for (const auto& d : detectors) {
            det_hist_.push_back(&PerfRegistry::instance().get_or_create(d->perf_stage_name()));
        }
        
        // Cache hot-path histogram pointers
        codec_to_book_hist_ = &PerfRegistry::instance().get_or_create(kStageCodecToBook);
        book_to_feature_hist_ = &PerfRegistry::instance().get_or_create(kStageBookToFeature);
    }

    void on_leader_frame(const FeatureFrame& frame) {
        std::lock_guard lock(mtx_);
        last_leader_frame_ = frame;
        if (leader_detector) {
            leader_detector->on_leader_frame(frame);
        }
        if (leader_tracker && last_frame_.mid > 0 && frame.mid > 0) {
            leader_tracker->update(frame.mid, last_frame_.mid);
        }
    }
    
    void on_trade(const Trade& t) {
        std::lock_guard lock(mtx_);
        stream->on_trade(t);
        for (size_t i = 0; i < detectors.size(); ++i) {
            LatencyTracer trace(*det_hist_[i]);
            detectors[i]->on_trade(t);
        }
        dirty_flags_ |= DirtyTrades;
    }

    void on_book_update(const OrderBookUpdate& u) {
        std::lock_guard lock(mtx_);
        {
            LatencyTracer trace(*codec_to_book_hist_);
            book->apply_update(u);
        }
        for (size_t i = 0; i < detectors.size(); ++i) {
            LatencyTracer trace(*det_hist_[i]);
            detectors[i]->on_book_update(u);
        }
        dirty_flags_ |= DirtyBook;
    }

    // IMarketDataListener — market data dispatch from MarketDataFeed
    void on_trade(const Ticker& /*ticker*/, const Trade& t) override { on_trade(t); }
    void on_trades(const Ticker& /*ticker*/, const std::vector<Trade>& trades) override {
        std::lock_guard lock(mtx_);
        for (const auto& t : trades) {
            stream->on_trade(t);
            for (size_t i = 0; i < detectors.size(); ++i) {
                LatencyTracer trace(*det_hist_[i]);
                detectors[i]->on_trade(t);
            }
        }
        dirty_flags_ |= DirtyTrades;
    }
    void on_orderbook_snapshot(const OrderBookSnapshot& snap) override {
        std::lock_guard lock(mtx_);
        
        // Sanity Check: if we already have data, verify consistency before applying.
        // Significant divergence (>3 levels) indicates a missed WS update gap.
        if (book->bid_levels() > 0 || book->ask_levels() > 0) {
            if (!book->is_consistent(snap, 3)) {
                LOG_WARN("OrderBook Sanity Check FAILED for {}. Local book diverged from snapshot. Resetting...", snap.ticker);
            } else {
                LOG_DEBUG("OrderBook Sanity Check PASSED for {}", snap.ticker);
            }
        }

        book->apply_snapshot(snap);
        dirty_flags_ |= DirtyBook;
    }
    void on_orderbook_update(const OrderBookUpdate& upd) override { on_book_update(upd); }
    void on_order_update(const OrderUpdate&) override {}
    void on_position_update(const PositionUpdate&) override {}
    void on_balance_update(const BalanceUpdate&) override {}
    void on_finres_update(const FinresUpdate&) override {}
    void on_error(const std::string&) override {}
    
    FeatureFrame tick(std::chrono::system_clock::time_point now) {
        std::lock_guard lock(mtx_);
        stream->update(now);
        
        // Lazy extraction: only extract if book or trades changed since last tick,
        // OR if enough time passed to warrant a fresh frame (e.g. for time-based features).
        // 500ms force-refresh ensures features don't stale completely if no trades.
        bool force_refresh = (now - last_extract_ > std::chrono::milliseconds(500));
        if (dirty_flags_ == 0 && !force_refresh) {
            return last_frame_;
        }

        {
            LatencyTracer trace(*book_to_feature_hist_);
            last_frame_ = extractor->extract(now);
        }
        
        if (last_leader_frame_.mid > 0) {
            last_frame_.leader_change_1s = last_leader_frame_.price_change_1s;
            last_frame_.leader_change_5s = last_leader_frame_.price_change_5s;
        }

        for (size_t i = 0; i < detectors.size(); ++i) {
            LatencyTracer trace(*det_hist_[i]);
            detectors[i]->on_frame(last_frame_);
        }
        
        dirty_flags_ = 0;
        last_extract_ = now;

        // DS-08: Push to chart history ring buffer for dashboard
        chart_history_.push(last_frame_);

        // Liquidity topology: compact density column aligned 1:1 with chart tick
        {
            const int64_t ts_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                last_frame_.timestamp.time_since_epoch()).count();
            book->get_top_levels(20, bid_buffer_, ask_buffer_);
            density_history_.push(build_density_column(ts_ms, bid_buffer_, ask_buffer_));
        }

        return last_frame_;
    }

    /// DS-08: Return a snapshot of the chart history (last 300 points).
    [[nodiscard]] std::vector<ChartPoint> chart_snapshot() const {
        std::lock_guard lock(mtx_);
        return chart_history_.snapshot();
    }

    /// Liquidity topology history aligned 1:1 with chart_snapshot() by ts.
    [[nodiscard]] std::vector<DensityColumn> density_snapshot() const {
        std::lock_guard lock(mtx_);
        return density_history_.snapshot();
    }

    /// DS-09: Return top N levels from the order book.
    [[nodiscard]] std::pair<std::vector<ObLevel>, std::vector<ObLevel>>
    ob_snapshot(int n_levels) const {
        std::lock_guard lock(mtx_);
        return book->get_top_levels(n_levels);
    }

private:
    mutable std::mutex mtx_;
    enum DirtyFlags {
        DirtyBook   = 1 << 0,
        DirtyTrades = 1 << 1
    };
    uint8_t dirty_flags_ = DirtyBook | DirtyTrades; // start dirty
    FeatureFrame last_frame_;
    FeatureFrame last_leader_frame_;
    std::chrono::system_clock::time_point last_extract_;
    ChartHistory chart_history_;
    DensityHistory density_history_;
    std::vector<ObLevel> bid_buffer_;
    std::vector<ObLevel> ask_buffer_;
    std::vector<HdrHistogram*> det_hist_; // Cached histogram pointers per detector
    HdrHistogram* codec_to_book_hist_;    // Cached histogram pointer for codec→book stage
    HdrHistogram* book_to_feature_hist_;  // Cached histogram pointer for book→feature stage
};

} // namespace trade_bot
