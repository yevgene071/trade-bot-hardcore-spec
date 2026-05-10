#include "DensityDetector.hpp"
#include "logger/Logger.hpp"

#include <cmath>

namespace trade_bot {

DensityDetector::DensityDetector(Ticker ticker,
                               SignalBus& bus,
                               const OrderBook& book,
                               const TickerUniverse& universe,
                               Config cfg)
    : ticker_(std::move(ticker))
    , bus_(bus)
    , book_(book)
    , universe_(universe)
    , cfg_(cfg)
    , avg_level_size_(Dema<double>::from_period(cfg.dema_period)) {}

DensityDetector::DensityDetector(Ticker ticker,
                               SignalBus& bus,
                               const OrderBook& book,
                               const TickerUniverse& universe)
    : DensityDetector(std::move(ticker), bus, book, universe, Config{}) {}

void DensityDetector::on_frame(const FeatureFrame& frame) {
    if (frame.ticker != ticker_) return;
    check_sticky_levels_(frame.timestamp);
}

void DensityDetector::on_trade(const Trade& trade) {
    auto now = trade.timestamp;

    // Sub-detector: DensityEating.
    // Match the trade against the tracked level at exactly its tick first; if
    // that misses, try the neighbouring ticks (-1, +1). Crypto venues sometimes
    // report aggressor prints with tiny rounding/slippage so the eat-through
    // event must still fire when the trade lands one tick off the level.
    // Side check is preserved (eating is the opposite side of resting density).
    // Fixes #116.
    const auto base_tick = PriceTick::from_price(trade.price, book_.price_increment());
    auto it = tracked_.find(base_tick);
    if (it == tracked_.end() || it->second.side == trade.side) {
        const PriceTick alt_minus{base_tick.ticks - 1};
        auto alt = tracked_.find(alt_minus);
        if (alt != tracked_.end() && alt->second.side != trade.side) {
            it = alt;
        } else {
            const PriceTick alt_plus{base_tick.ticks + 1};
            alt = tracked_.find(alt_plus);
            if (alt != tracked_.end() && alt->second.side != trade.side) {
                it = alt;
            }
        }
    }

    if (it != tracked_.end() && it->second.side != trade.side) {
        auto& meta = it->second;
        meta.eaten_volume += trade.size;
        meta.print_count++;
        meta.last_hit = now;
        
        double eaten_ratio = meta.eaten_volume / meta.initial_size;
        if (eaten_ratio >= cfg_.eating_ratio_threshold && 
            meta.print_count >= cfg_.eating_min_prints) {
            
            Signal s {
                .kind = SignalKind::DensityEating,
                .timestamp = now,
                .ticker = ticker_,
                .price = trade.price,
                .confidence = std::min(1.0, eaten_ratio),
                .payload = nlohmann::json{
                    {"side", meta.side == Side::Buy ? "Bid" : "Ask"},
                    {"eaten_ratio", eaten_ratio},
                    {"prints", meta.print_count}
                }
            };
            bus_.publish(s);
        }
    }
}

void DensityDetector::on_book_update(const OrderBookUpdate& update) {
    if (update.ticker != ticker_) return;

    auto now = update.ts;
    auto mid = book_.mid();
    if (!mid) return;

    // Update rolling average of level sizes from current book
    // To avoid too many updates, we only sample some levels
    // (Simplified: update from each level change)
    for (const auto& level : update.changes) {
        if (level.size > 0) {
            avg_level_size_.update(level.size);
        }
    }

    double avg_size = avg_level_size_.value();
    double min_size_calibrated = universe_.density_min_size_usd(ticker_, cfg_.min_size_usd);

    for (const auto& level : update.changes) {
        const auto tick = PriceTick::from_price(level.price, book_.price_increment());
        
        if (level.size > 0) {
            // Potential density?
            double dist_bps = std::abs(level.price - *mid) / (*mid) * 10000.0;
            
            if (level.size >= cfg_.min_size_vs_avg * avg_size &&
                level.size * level.price >= min_size_calibrated &&
                dist_bps >= cfg_.min_distance_bps &&
                dist_bps <= cfg_.max_distance_bps) {
                
                if (tracked_.find(tick) == tracked_.end()) {
                    tracked_[tick] = LevelMeta {
                        .side = level.side,
                        .initial_size = level.size,
                        .first_seen = now,
                        .emitted = false,
                        .eaten_volume = 0.0,
                        .print_count = 0,
                        .last_hit = {}
                    };
                }
            }
        } else {
            // Level removed
            auto it = tracked_.find(tick);
            if (it != tracked_.end()) {
                auto age = std::chrono::duration_cast<std::chrono::milliseconds>(now - it->second.first_seen);
                if (age < cfg_.fake_threshold) {
                    Signal s {
                        .kind = SignalKind::DensityRemoved,
                        .timestamp = now,
                        .ticker = ticker_,
                        .price = level.price,
                        .confidence = 1.0,
                        .payload = nlohmann::json{{"fake", true}, {"age_ms", age.count()}}
                    };
                    bus_.publish(s);
                }
                tracked_.erase(it);
            }
        }
    }
}

void DensityDetector::check_sticky_levels_(std::chrono::system_clock::time_point now) {
    for (auto& [tick, meta] : tracked_) {
        if (meta.emitted) continue;

        auto age = std::chrono::duration_cast<std::chrono::milliseconds>(now - meta.first_seen);
        if (age >= cfg_.sticky_duration) {
            meta.emitted = true;
            
            Signal s {
                .kind = SignalKind::DensityDetected,
                .timestamp = now,
                .ticker = ticker_,
                .price = tick.to_price(book_.price_increment()),
                .confidence = 1.0,
                .payload = nlohmann::json{
                    {"side", meta.side == Side::Buy ? "Bid" : "Ask"},
                    {"size", meta.initial_size},
                    {"age_ms", age.count()}
                }
            };
            bus_.publish(s);
        }
    }
}

} // namespace trade_bot
