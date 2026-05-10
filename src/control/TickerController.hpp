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

namespace trade_bot {

/**
 * T3-INTEGRATION: Container for all per-ticker market data and signal processing components.
 * Implements IMarketDataListener so it can be registered directly with MarketDataFeed.
 */
struct TickerController : public IMarketDataListener {
    std::unique_ptr<OrderBook> book;
    std::unique_ptr<TradeStream> stream;
    std::unique_ptr<FeatureExtractor> extractor;
    
    // Detectors
    std::vector<std::unique_ptr<IDetector>> detectors;
    
    TickerController(Ticker ticker, 
                     SignalBus& bus, 
                     const TickerUniverse& universe,
                     const ClusterSnapshotManager& cluster_mgr) {
        
        auto meta = universe.meta(ticker).value_or(TickerMeta{0.01, 1e-6, 0, 0});
        
        book = std::make_unique<OrderBook>(ticker, meta.price_increment, meta.size_increment); 
        stream = std::make_unique<TradeStream>(ticker);
        extractor = std::make_unique<FeatureExtractor>(ticker);
        extractor->set_sources(book.get(), stream.get(), nullptr);
        
        detectors.push_back(std::make_unique<DensityDetector>(ticker, bus, *book, universe));
        detectors.push_back(std::make_unique<IcebergDetector>(ticker, bus, *book, universe));
        detectors.push_back(std::make_unique<TapeAnalyzer>(ticker, bus, *book, *stream));
        
        auto ld = std::make_unique<LevelDetector>(ticker, bus, *book, cluster_mgr);
        const auto* ld_ptr = ld.get();
        detectors.push_back(std::move(ld));
        
        detectors.push_back(std::make_unique<ApproachAnalyzer>(ticker, bus, *book, *ld_ptr));
    }
    
    void on_trade(const Trade& t) {
        stream->on_trade(t);
        for (auto& d : detectors) d->on_trade(t);
    }

    void on_book_update(const OrderBookUpdate& u) {
        book->apply_update(u);
        for (auto& d : detectors) d->on_book_update(u);
    }

    // IMarketDataListener — market data dispatch from MarketDataFeed
    void on_trade(const Ticker& /*ticker*/, const Trade& t) override { on_trade(t); }
    void on_trades(const Ticker& /*ticker*/, const std::vector<Trade>& trades) override {
        for (const auto& t : trades) on_trade(t);
    }
    void on_orderbook_snapshot(const OrderBookSnapshot& snap) override {
        book->apply_snapshot(snap);
    }
    void on_orderbook_update(const OrderBookUpdate& upd) override { on_book_update(upd); }
    void on_order_update(const OrderUpdate&) override {}
    void on_position_update(const PositionUpdate&) override {}
    void on_balance_update(const BalanceUpdate&) override {}
    void on_finres_update(const FinresUpdate&) override {}
    void on_error(const std::string&) override {}
    
    FeatureFrame tick(std::chrono::system_clock::time_point now) {
        stream->update(now);
        auto frame = extractor->extract(now);
        for (auto& d : detectors) d->on_frame(frame);
        return frame;
    }
};

} // namespace trade_bot
