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

#include <memory>

namespace trade_bot {

/**
 * T3-INTEGRATION: Container for all per-ticker market data and signal processing components.
 */
struct TickerController {
    std::unique_ptr<OrderBook> book;
    std::unique_ptr<TradeStream> stream;
    std::unique_ptr<FeatureExtractor> extractor;
    
    // Detectors
    std::vector<std::unique_ptr<IDetector>> detectors;
    
    TickerController(Ticker ticker, 
                     SignalBus& bus, 
                     const TickerUniverse& universe,
                     const ClusterSnapshotManager& cluster_mgr) {
        
        book = std::make_unique<OrderBook>(ticker, 0.01, 1e-6); 
        stream = std::make_unique<TradeStream>(ticker);
        extractor = std::make_unique<FeatureExtractor>(ticker);
        extractor->set_sources(book.get(), stream.get(), nullptr);
        
        detectors.push_back(std::make_unique<DensityDetector>(ticker, bus, *book, universe));
        detectors.push_back(std::make_unique<IcebergDetector>(ticker, bus, *book, universe));
        detectors.push_back(std::make_unique<TapeAnalyzer>(ticker, bus, *book, *stream));
        detectors.push_back(std::make_unique<LevelDetector>(ticker, bus, *book, cluster_mgr));
        detectors.push_back(std::make_unique<ApproachAnalyzer>(ticker, bus, *book, static_cast<const LevelDetector&>(*detectors.back())));
    }
    
    void on_trade(const Trade& t) {
        stream->on_trade(t);
        for (auto& d : detectors) d->on_trade(t);
    }
    
    void on_book_update(const OrderBookUpdate& u) {
        book->apply_update(u);
        for (auto& d : detectors) d->on_book_update(u);
    }
    
    FeatureFrame tick(std::chrono::system_clock::time_point now) {
        stream->update(now);
        auto frame = extractor->extract(now);
        for (auto& d : detectors) d->on_frame(frame);
        return frame;
    }
};

} // namespace trade_bot
