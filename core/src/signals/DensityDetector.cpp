#include "DensityDetector.hpp"
#include "logger/Logger.hpp"
#include "perf/TraceContext.hpp"
#include "perf/PerfRegistry.hpp"

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
    if (!frame.valid) return;
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

        // BUG-2 FIX: reset eating stats when the gap since the last hit exceeds
        // eating_window. Eating is a sustained-pressure event; volume accumulated
        // in an earlier burst must not carry over into a new, unrelated burst.
        if (meta.last_hit != std::chrono::system_clock::time_point{} &&
            (now - meta.last_hit) > cfg_.eating_window) {
            meta.eaten_volume = 0.0;
            meta.print_count = 0;
            meta.eating_emitted = false; // new burst — allow one signal again
        }

        meta.eaten_volume += trade.size;
        meta.print_count++;
        meta.last_hit = now;

        double eaten_ratio = meta.eaten_volume / meta.initial_size;
        if (!meta.eating_emitted &&
            eaten_ratio >= cfg_.eating_ratio_threshold &&
            meta.print_count >= cfg_.eating_min_prints) {
            meta.eating_emitted = true;

            Signal s {
                .kind = SignalKind::DensityEating,
                .timestamp = now,
                .ticker = ticker_,
                .price = trade.price,
                .confidence = std::min(1.0, eaten_ratio),
                .payload = {
                    .side = meta.side == Side::Buy ? "Bid" : "Ask",
                    .size = meta.initial_size - meta.eaten_volume,
                    .original_size = meta.initial_size,
                    .remaining_size = meta.initial_size - meta.eaten_volume,
                    .eaten_ratio = eaten_ratio,
                    .prints = meta.print_count
                }
            };
            s.trigger_trace_id = current_trace_context().trace_id;
            bus_.publish(s);
        }
    }
}

void DensityDetector::on_book_update(const OrderBookUpdate& update) {
    if (update.ticker != ticker_) return;

    auto now = update.ts;

    // Pass 1: removals — mid is not required.
    // NEW-01 fix: former `if (!mid) return` blocked this path during book rebuild,
    // leaking stale entries in tracked_ that would never be cleaned up.
    for (const auto& level : update.changes) {
        if (level.size > 0) continue;
        const auto tick = PriceTick::from_price(level.price, book_.price_increment());
        auto it = tracked_.find(tick);
        if (it == tracked_.end()) continue;

        auto age = std::chrono::duration_cast<std::chrono::milliseconds>(now - it->second.first_seen);
        if (age < cfg_.fake_threshold) {
            Signal s {
                .kind = SignalKind::DensityRemoved,
                .timestamp = now,
                .ticker = ticker_,
                .price = level.price,
                .confidence = 1.0,
                .payload = {.age_ms = static_cast<int>(age.count()), .fake = true}
            };
            s.trigger_trace_id = current_trace_context().trace_id;
            bus_.publish(s);
        } else {
            // NEW-04 fix: emit for all real removals (age >= fake_threshold),
            // not only levels that previously fired DensityDetected.
            Signal s {
                .kind = SignalKind::DensityRemoved,
                .timestamp = now,
                .ticker = ticker_,
                .price = level.price,
                .confidence = 1.0,
                .payload = {.age_ms = static_cast<int>(age.count()), .fake = false}
            };
            s.trigger_trace_id = current_trace_context().trace_id;
            bus_.publish(s);
        }
        tracked_.erase(it);
    }

    // Pass 2: additions — mid required for distance filtering.
    auto mid = book_.mid();
    if (!mid) return;

    double min_size_calibrated = universe_.density_min_size_usd(ticker_, cfg_.min_size_usd);

    for (const auto& level : update.changes) {
        // X1: only update DEMA with levels that have meaningful USD value to avoid
        // dust levels dragging the average down and falsely lowering the detection threshold
        if (level.size > 0 && level.price > 0.0 &&
            level.size * level.price >= min_size_calibrated * 0.05) {
            avg_level_size_.update(level.size);
        }
    }

    double avg_size = avg_level_size_.value();

    for (const auto& level : update.changes) {
        if (level.size == 0) continue;
        const auto tick = PriceTick::from_price(level.price, book_.price_increment());

        double dist_bps = std::abs(level.price - *mid) / (*mid) * 10000.0;

        if (level.size >= cfg_.min_size_vs_avg * avg_size &&
            level.size * level.price >= min_size_calibrated &&
            dist_bps >= cfg_.min_distance_bps &&
            dist_bps <= cfg_.max_distance_bps) {

            auto it = tracked_.find(tick);
            if (it == tracked_.end()) {
                tracked_[tick] = LevelMeta {
                    .side = level.side,
                    .initial_size = level.size,
                    .first_seen = now,
                    .emitted = false,
                    .eaten_volume = 0.0,
                    .print_count = 0,
                    .last_hit = {}
                };
            } else if (level.size >= it->second.initial_size) {
                // X2: level refilled/grew — re-arm sticky timer so a new DensityDetected
                // can fire if the large order persists again for sticky_duration
                it->second.initial_size = level.size;
                it->second.first_seen   = now;
                it->second.emitted      = false;
                it->second.eaten_volume = 0.0;
                it->second.print_count  = 0;
                it->second.eating_emitted = false;
            } else {
                // NEW-03 fix: partial cancellation — reset eating baseline to current
                // book size so eaten_ratio measures against the actual residual, not
                // the original level size.
                it->second.initial_size = level.size;
                it->second.eaten_volume = 0.0;
                it->second.print_count = 0;
                // M-04: do NOT reset eating_emitted if already fired — a partial
                // cancel by the MM does not undo detected eating; resetting would
                // allow a spurious second DensityEating on the same level.
            }
        }
    }
}

void DensityDetector::check_sticky_levels_(std::chrono::system_clock::time_point now) {
    // absl::flat_hash_map::begin() segfaults with ASan when the map is empty
    // (null ctrl_ pointer in generation tracking). Guard against that.
    if (tracked_.empty()) return;

    const auto mid = book_.mid();
    const double price_inc = book_.price_increment();

    for (auto it = tracked_.begin(); it != tracked_.end(); ) {
        auto& [tick, meta] = *it;

        // X3: evict levels that drifted outside the detection range
        if (mid && *mid > 0.0) {
            const double price   = tick.to_price(price_inc);
            const double dist_bps = std::abs(price - *mid) / (*mid) * 10000.0;
            if (dist_bps > cfg_.max_distance_bps) {
                tracked_.erase(it++); // X3: erase returns void in this absl version
                continue;
            }
        }

        if (!meta.emitted) {
            auto age = std::chrono::duration_cast<std::chrono::milliseconds>(now - meta.first_seen);
            if (age >= cfg_.sticky_duration) {
                meta.emitted = true;

                Signal s {
                    .kind = SignalKind::DensityDetected,
                    .timestamp = now,
                    .ticker = ticker_,
                    .price = tick.to_price(price_inc),
                    .confidence = 1.0,
                    .payload = {.side = meta.side == Side::Buy ? "Bid" : "Ask", .size = meta.initial_size, .age_ms = static_cast<int>(age.count())}
                };
                s.trigger_trace_id = current_trace_context().trace_id;
                bus_.publish(s);
            }
        }
        ++it;
    }
}

} // namespace trade_bot
