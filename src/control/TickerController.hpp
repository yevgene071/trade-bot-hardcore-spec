#pragma once

#include "marketdata/OrderBook.hpp"
#include "marketdata/TradeStream.hpp"
#include "features/FeatureExtractor.hpp"
#include "signals/DensityDetector.hpp"
#include "signals/IcebergDetector.hpp"
#include "signals/TapeAnalyzer.hpp"
#include "signals/LevelDetector.hpp"
#include "signals/ApproachAnalyzer.hpp"
#include "signals/LeaderSignal.hpp"
#include "transport/MarketDataFeed.hpp"

#include <memory>
#include <mutex>

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
        
        detectors.push_back(std::make_unique<DensityDetector>(ticker, bus, *book, universe));
        detectors.push_back(std::make_unique<IcebergDetector>(ticker, bus, *book, universe));
        detectors.push_back(std::make_unique<TapeAnalyzer>(ticker, bus, *book, *stream));
        
        auto ld = std::make_unique<LevelDetector>(ticker, bus, *book, cluster_mgr);
        const auto* ld_ptr = ld.get();
        detectors.push_back(std::move(ld));
        
        detectors.push_back(std::make_unique<ApproachAnalyzer>(ticker, bus, *book, *ld_ptr));

        if (leader_tracker && leader_ticker) {
            auto ls = std::make_unique<LeaderSignal>(ticker, *leader_ticker, bus, *leader_tracker);
            leader_detector = ls.get();
            detectors.push_back(std::move(ls));
        }
    }

    void on_leader_frame(const FeatureFrame& frame) {
        std::lock_guard lock(mtx_);
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
        for (auto& d : detectors) d->on_trade(t);
        dirty_flags_ |= DirtyTrades;
    }

    void on_book_update(const OrderBookUpdate& u) {
        std::lock_guard lock(mtx_);
        book->apply_update(u);
        for (auto& d : detectors) d->on_book_update(u);
        dirty_flags_ |= DirtyBook;
    }

    // IMarketDataListener — market data dispatch from MarketDataFeed
    void on_trade(const Ticker& /*ticker*/, const Trade& t) override { on_trade(t); }
    void on_trades(const Ticker& /*ticker*/, const std::vector<Trade>& trades) override {
        std::lock_guard lock(mtx_);
        for (const auto& t : trades) {
            stream->on_trade(t);
            for (auto& d : detectors) d->on_trade(t);
        }
        dirty_flags_ |= DirtyTrades;
    }
    void on_orderbook_snapshot(const OrderBookSnapshot& snap) override {
        std::lock_guard lock(mtx_);
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

        last_frame_ = extractor->extract(now);
        for (auto& d : detectors) d->on_frame(last_frame_);
        
        dirty_flags_ = 0;
        last_extract_ = now;
        return last_frame_;
    }

private:
    std::mutex mtx_;
    enum DirtyFlags {
        DirtyBook   = 1 << 0,
        DirtyTrades = 1 << 1
    };
    uint8_t dirty_flags_ = DirtyBook | DirtyTrades; // start dirty
    FeatureFrame last_frame_;
    std::chrono::system_clock::time_point last_extract_;
};

} // namespace trade_bot
