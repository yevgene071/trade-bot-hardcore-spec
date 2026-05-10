#include "IcebergDetector.hpp"
#include "logger/Logger.hpp"

#include <absl/container/flat_hash_map.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace trade_bot {

IcebergDetector::IcebergDetector(Ticker ticker,
                                 SignalBus& bus,
                                 const OrderBook& book,
                                 const TickerUniverse& universe,
                                 const Config& cfg)
    : ticker_(std::move(ticker)), bus_(bus), book_(book), universe_(universe), cfg_(cfg) {}

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

    // T4-LIFECYCLE: Prune stale level stats to prevent memory bloat (#142)
    if (now - last_prune_ > std::chrono::minutes(1)) {
        for (auto it = levels_.begin(); it != levels_.end(); ) {
            if (now - it->second.last_refill > std::chrono::minutes(5)) {
                it = levels_.erase(it);
            } else {
                ++it;
            }
        }
        
        // Also prune last_sizes
        if (last_sizes_.size() > 1000) {
            last_sizes_.clear(); // aggressive reset
        }
        last_prune_ = now;
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

    // T4-PERF: Reuse member maps to avoid allocations (#159)
    buy_vol_by_tick_.clear();
    sell_vol_by_tick_.clear();

    for (const auto& t : trade_history_) {
        auto delta = std::chrono::duration_cast<std::chrono::milliseconds>(now - t.ts);
        if (std::abs(delta.count()) > cfg_.event_join_window.count()) continue;
        const auto ttick = PriceTick::from_price(t.price, price_inc);
        if (t.side == Side::Buy) buy_vol_by_tick_[ttick] += t.size;
        else                      sell_vol_by_tick_[ttick] += t.size;
    }

    for (const auto& level : update.changes) {
        const auto tick = PriceTick::from_price(level.price, price_inc);

        double size_before = 0.0;
        auto it_last = last_sizes_.find(tick);
        if (it_last != last_sizes_.end()) {
            size_before = it_last->second;
        } else {
            last_sizes_[tick] = level.size;
            continue;
        }

        double size_after = level.size;
        last_sizes_[tick] = size_after;

        // O(1) lookup: trades matched against this level
        double matched_trade_vol = 0.0;
        if (level.side == Side::Sell) {
            auto it = buy_vol_by_tick_.find(tick);
            if (it != buy_vol_by_tick_.end()) matched_trade_vol = it->second;
        } else {
            auto it = sell_vol_by_tick_.find(tick);
            if (it != sell_vol_by_tick_.end()) matched_trade_vol = it->second;
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
    double denom = (p * L1 + (1.0 - p) * L0);
    
    // T4-MATH: Prevent division by zero and probability leakage (#141)
    if (denom < 1e-12) {
        stats.posterior = is_refill ? 0.99 : 0.01;
    } else {
        stats.posterior = std::clamp((p * L1) / denom, 0.0001, 0.9999);
    }
}

} // namespace trade_bot
