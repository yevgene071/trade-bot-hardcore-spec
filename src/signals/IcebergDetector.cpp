#include "IcebergDetector.hpp"
#include "logger/Logger.hpp"

#include <algorithm>
#include <cmath>

namespace trade_bot {

IcebergDetector::IcebergDetector(Ticker ticker,
                               SignalBus& bus,
                               const OrderBook& book,
                               const TickerUniverse& universe,
                               Config cfg)
    : ticker_(std::move(ticker))
    , bus_(bus)
    , book_(book)
    , universe_(universe)
    , cfg_(cfg) {}

IcebergDetector::IcebergDetector(Ticker ticker,
                               SignalBus& bus,
                               const OrderBook& book,
                               const TickerUniverse& universe)
    : IcebergDetector(std::move(ticker), bus, book, universe, Config{}) {}

void IcebergDetector::on_frame(const FeatureFrame& frame) {
    if (frame.ticker != ticker_) return;

    // Prune history
    auto now = frame.timestamp;
    while (!trade_history_.empty() && (now - trade_history_.front().ts) > cfg_.event_join_window) {
        trade_history_.pop_front();
    }
}

void IcebergDetector::on_trade(const Trade& trade) {
    // Assumption: all trades passed to this detector instance are for ticker_
    trade_history_.push_back({trade.timestamp, trade.price, trade.size, trade.side});
    if (trade_history_.size() > kMaxHistory) trade_history_.pop_front();
}

void IcebergDetector::on_book_update(const OrderBookUpdate& update) {
    if (update.ticker != ticker_) return;

    auto now = update.ts;
    const auto price_inc = book_.price_increment();

    for (const auto& level : update.changes) {
        const auto tick = PriceTick::from_price(level.price, price_inc);
        
        double size_before = 0.0;
        auto it_last = last_sizes_.find(tick);
        if (it_last != last_sizes_.end()) {
            size_before = it_last->second;
        } else {
            // First time we see this level, or it was 0
            // Can't reliably detect iceberg on first see
            last_sizes_[tick] = level.size;
            continue;
        }

        double size_after = level.size;
        last_sizes_[tick] = size_after;

        // Only interested if size decreased or stayed same while trades happened
        // Or if size increased while trades happened (refill > trade)
        
        // Find matching trades for this price and side
        // Trade side is Buy -> matched against Sell level
        double matched_trade_vol = 0.0;
        for (auto& t : trade_history_) {
            // Check ±100ms window
            auto delta = std::chrono::duration_cast<std::chrono::milliseconds>(now - t.ts);
            if (std::abs(delta.count()) <= cfg_.event_join_window.count()) {
                // Side matching: if it's a Buy trade, it hits Sell levels.
                // level.side is the side of the book level.
                if (std::abs(t.price - level.price) < price_inc * 0.5 && 
                    ((t.side == Side::Buy && level.side == Side::Sell) ||
                     (t.side == Side::Sell && level.side == Side::Buy))) {
                    matched_trade_vol += t.size;
                }
            }
        }

        if (matched_trade_vol > 0.0) {
            process_refill_(level.price, level.side, matched_trade_vol, size_before, size_after, now);
        }
    }
}

void IcebergDetector::process_refill_(double price, Side side, double trade_size, 
                                     double size_before, double size_after, 
                                     std::chrono::system_clock::time_point ts) {
    const auto price_inc = book_.price_increment();
    const auto tick = PriceTick::from_price(price, price_inc);
    
    double visible_decrease = size_before - size_after;
    bool is_refill = (trade_size > visible_decrease) && (size_after >= size_before * cfg_.size_retention_ratio);
    
    auto& stats = levels_[tick];
    if (stats.posterior == 0.0) {
        stats.posterior = cfg_.prior;
    }
    
    update_bayesian_(stats, is_refill);
    
    if (is_refill) {
        stats.refill_count++;
        stats.total_eaten_vol += trade_size;
        stats.last_refill = ts;
    }

    bool should_emit = false;
    if (cfg_.posterior_threshold > 0.0) {
        should_emit = (stats.posterior >= cfg_.posterior_threshold);
    } else {
        // legacy counter fallback
        should_emit = (stats.refill_count >= cfg_.evidence_count_min);
    }

    // Additional condition: total consumed volume
    if (should_emit && !stats.emitted) {
        if (stats.total_eaten_vol * price >= cfg_.iceberg_min_size_usd) {
            stats.emitted = true;
            
            Signal s {
                .kind = SignalKind::IcebergSuspected,
                .timestamp = ts,
                .ticker = ticker_,
                .price = price,
                .confidence = stats.posterior,
                .payload = nlohmann::json{
                    {"side", side == Side::Buy ? "Bid" : "Ask"},
                    {"total_eaten_usd", stats.total_eaten_vol * price},
                    {"refill_events", stats.refill_count}
                }
            };
            bus_.publish(s);
        }
    }
}

void IcebergDetector::update_bayesian_(LevelStats& stats, bool is_refill) {
    double L1 = is_refill ? cfg_.likelihood_iceberg : (1.0 - cfg_.likelihood_iceberg);
    double L0 = is_refill ? cfg_.likelihood_not_iceberg : (1.0 - cfg_.likelihood_not_iceberg);
    
    double p = stats.posterior;
    stats.posterior = (p * L1) / (p * L1 + (1.0 - p) * L0);
}

} // namespace trade_bot
