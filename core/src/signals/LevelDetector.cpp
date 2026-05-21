#include "LevelDetector.hpp"
#include "ApproachAnalyzer.hpp"
#include "logger/Logger.hpp"
#include "perf/TraceContext.hpp"

#include <cmath>
#include <algorithm>
#include <iostream>
#include <utility>

namespace trade_bot {

LevelDetector::LevelDetector(Ticker ticker,
                             SignalBus& bus,
                             const OrderBook& book,
                             const ClusterSnapshotManager& cluster_mgr,
                             const Config& cfg)
    : ticker_(std::move(ticker)), bus_(bus), book_(book), cluster_mgr_(cluster_mgr), cfg_(cfg) {}

LevelDetector::LevelDetector(Ticker ticker,
                           SignalBus& bus,
                           const OrderBook& book,
                           const ClusterSnapshotManager& cluster_mgr)
    : LevelDetector(std::move(ticker), bus, book, cluster_mgr, Config{}) {}

void LevelDetector::on_frame(const FeatureFrame& frame) {
    if (frame.ticker != ticker_) return;
    if (!frame.valid) return;

    update_extremes_(frame);
    
    // Rebuild levels every minute
    if (frame.timestamp - last_rebuild_ >= std::chrono::minutes(1)) {
        rebuild_levels_(frame.timestamp);
        last_rebuild_ = frame.timestamp;
    }

    check_approaches_(frame);
}

void LevelDetector::rebuild() {
    rebuild_levels_(std::chrono::system_clock::now()); // AA3: non-deterministic; prefer rebuild(ts) in replay
}

void LevelDetector::rebuild(std::chrono::system_clock::time_point now) { // AA3
    rebuild_levels_(now);
}

void LevelDetector::on_trade(const Trade& /*trade*/) {
}

void LevelDetector::on_book_update(const OrderBookUpdate& /*update*/) {
}

void LevelDetector::update_extremes_(const FeatureFrame& frame) {
    mid_history_.push_back({frame.timestamp, frame.mid});

    // AA4: multi-scale windows come from config (no longer hardcoded {5,25,75})
    for (const size_t hw : cfg_.extreme_half_windows) {
        const size_t need = 2 * hw + 1;
        if (mid_history_.size() < need) continue;
        const size_t i = mid_history_.size() - (hw + 1);

        double cur = mid_history_[i].second;
        bool is_max = true, is_min = true;
        for (size_t j = i - hw; j <= i + hw; ++j) {
            if (j == i) continue;
            if (mid_history_[j].second >= cur) is_max = false;
            if (mid_history_[j].second <= cur) is_min = false;
        }

        if (is_max || is_min) {
            double start = mid_history_[i - hw].second;
            double end   = mid_history_[i + hw].second;
            double magnitude_bps = std::max(std::abs(cur - start), std::abs(cur - end)) / cur * 10000.0;

            if (magnitude_bps >= cfg_.min_reversal_bps) {
                auto ts = mid_history_[i].first;
                
                bool found = false;
                for (int k = static_cast<int>(extremes_.size()) - 1; k >= 0; --k) {
                    if (extremes_[k].ts == ts) {
                        found = true;
                        break;
                    }
                    if (extremes_[k].ts < ts) break; // Ordered by ts
                }
                
                if (!found) {
                     extremes_.push_back({cur, ts, is_max});
                }
            }
        }
    }
}

void LevelDetector::rebuild_levels_(std::chrono::system_clock::time_point now) {
    if (extremes_.size() < cfg_.touches_min) {
        return;
    }

    std::vector<double> prices(extremes_.size());
    for (size_t i = 0; i < extremes_.size(); ++i) {
        prices[i] = extremes_[i].price;
    }

    double mid = book_.mid().value_or(0.0);
    if (mid == 0.0 && !mid_history_.empty()) mid = mid_history_.back().second;
    if (mid == 0.0) return;
    
    double eps = mid * (cfg_.cluster_tolerance_bps / kBpsBase);

    auto clusters = Dbscan1D::cluster(prices, eps, cfg_.touches_min);
    
    std::vector<Level> new_levels;
    
    Kde::DataSet kde_data;
    auto m15 = cluster_mgr_.get(ticker_, "M15");
    if (m15) {
        kde_data.values.resize(m15->items.size());
        kde_data.weights.resize(m15->items.size());
        for (size_t i = 0; i < m15->items.size(); ++i) {
            kde_data.values[i] = m15->items[i].price;
            kde_data.weights[i] = m15->items[i].ask_size + m15->items[i].bid_size;
            kde_data.total_weight += kde_data.weights[i];
        }
    }

    double h = Kde::silverman_bandwidth(kde_data) * cfg_.kde_smoothness;

    // ---- Pass 1: per-cluster centroid + raw KDE density. ----
    struct Pending {
        double                                centroid;
        std::size_t                           cluster_size;
        double                                kde_val;
        std::chrono::system_clock::time_point last_ts;
    };
    // Precompute sorted (price, ts) for O(logE) range queries inside the cluster loop
    std::vector<std::pair<double, std::chrono::system_clock::time_point>> sorted_extremes;
    sorted_extremes.reserve(extremes_.size());
    for (size_t i = 0; i < extremes_.size(); ++i) sorted_extremes.emplace_back(extremes_[i].price, extremes_[i].ts);
    std::sort(sorted_extremes.begin(), sorted_extremes.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });

    std::vector<Pending> pending;
    pending.reserve(clusters.size());
    double max_kde = 0.0;

    for (const auto& c : clusters) {
        double centroid = std::accumulate(c.begin(), c.end(), 0.0) / static_cast<double>(c.size());

        double kde_val = 0.0;
        if (!kde_data.values.empty()) {
            auto dens = Kde::estimate(kde_data, {centroid}, h);
            kde_val = dens[0];
        }

        // O(logE) range query for extremes near centroid
        auto lo = std::lower_bound(sorted_extremes.begin(), sorted_extremes.end(),
                                   std::make_pair(centroid - eps, std::chrono::system_clock::time_point{}),
                                   [](const auto& a, const auto& b) { return a.first < b.first; });
        auto hi = std::upper_bound(lo, sorted_extremes.end(),
                                   std::make_pair(centroid + eps, std::chrono::system_clock::time_point::max()),
                                   [](const auto& a, const auto& b) { return a.first < b.first; });
        auto last_ts = std::chrono::system_clock::time_point{};
        for (auto it = lo; it != hi; ++it) {
            if (it->second > last_ts) last_ts = it->second;
        }

        if (kde_val > max_kde) max_kde = kde_val;
        pending.push_back({centroid, c.size(), kde_val, last_ts});
    }

    // ---- Pass 2: build Level entries with normalized kde_score. ----
    for (const auto& p : pending) {
        const double age_min = std::chrono::duration_cast<std::chrono::minutes>(
            now - p.last_ts).count();
        const double recency_score = std::exp(-age_min / 60.0);
        const double touches_score = std::min(
            1.0, static_cast<double>(p.cluster_size) / 5.0);
        const double kde_score = max_kde > 0.0 ? (p.kde_val / max_kde) : 0.0;

        const double confidence =
            0.4 * touches_score + 0.4 * kde_score + 0.2 * recency_score;

        new_levels.push_back(Level{
            .price       = p.centroid,
            .touches     = static_cast<int>(p.cluster_size),
            .kde_score   = kde_score,
            .last_touch  = p.last_ts,
            .created_at  = now,
            .source      = "extreme",
            .confidence  = confidence
        });
    }

    // GAP-2 FIX: Emit LevelFormed for levels that appear in this rebuild
    // but were not present in the previous active set (within cluster tolerance).
    for (const auto& nl : new_levels) {
        bool is_new = true;
        for (const auto& ol : active_levels_) {
            if (std::abs(nl.price - ol.price) < eps) { is_new = false; break; }
        }
        if (is_new) {
            Signal s {
                .kind = SignalKind::LevelFormed,
                .timestamp = now,
                .ticker = ticker_,
                .price = nl.price,
                .confidence = nl.confidence,
                .payload = {.touches = nl.touches}
            };
            s.trigger_trace_id = current_trace_context().trace_id;
            bus_.publish(s);
        }
    }

    active_levels_ = std::move(new_levels);

    // AA5: prune approach state for levels that were evicted from the active set
    for (auto it = current_approaches_.begin(); it != current_approaches_.end(); ) {
        bool still_active = false;
        for (const auto& lv : active_levels_) {
            if (std::abs(lv.price - it->second.level_price) < eps) { still_active = true; break; }
        }
        if (still_active) {
            ++it; // AA5: erase returns void in this absl version
        } else {
            current_approaches_.erase(it++);
        }
    }
}

void LevelDetector::check_approaches_(const FeatureFrame& frame) {
    if (frame.mid <= 0.0) return;
    const double price_inc = book_.price_increment();
    for (const auto& level : active_levels_) {
        double dist_bps = (frame.mid - level.price) / frame.mid * 10000.0;
        const auto tick = PriceTick::from_price(level.price, price_inc);
        
        if (std::abs(dist_bps) <= cfg_.approach_trigger_bps) {
            if (current_approaches_.find(tick) == current_approaches_.end()) {
                SignalPayload payload;
                payload.dist_bps = dist_bps;
                payload.touches = level.touches;
                // GAP-02: level age at approach time, in ms. Used by BounceFromDensity
                // to enforce max_level_age (§1.3: level must be ≤ 60s old).
                payload.age_ms = static_cast<int>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        frame.timestamp - level.created_at).count());

                if (analyzer_) {
                    auto analysis = analyzer_->analyze(level.price, frame.timestamp);
                    payload.speed_bps = analysis.speed_bps_sec;
                    switch (analysis.type) {
                        case ApproachAnalyzer::ApproachType::Impulse:       payload.approach_type = "impulse"; break;
                        case ApproachAnalyzer::ApproachType::Slow:          payload.approach_type = "slow"; break;
                        case ApproachAnalyzer::ApproachType::Consolidation: payload.approach_type = "consolidation"; break;
                        default:                                            payload.approach_type = "unknown"; break;
                    }
                }

                Signal s {
                    .kind = SignalKind::LevelApproach,
                    .timestamp = frame.timestamp,
                    .ticker = ticker_,
                    .price = level.price,
                    .confidence = level.confidence,
                    .payload = payload
                };
                s.trigger_trace_id = current_trace_context().trace_id;
                bus_.publish(s);
                current_approaches_[tick] = {
                    .level_price = level.price,
                    .start_ts    = frame.timestamp,
                    .above_level = frame.mid > level.price,
                    .entry_mid   = frame.mid
                };
            }
        } else {
            // GAP-2 FIX: price left the approach zone — emit LevelRejection or LevelBreak.
            // Rejection: price bounced back to same side it came from.
            // Break: price crossed through to the other side.
            auto it = current_approaches_.find(tick);
            if (it != current_approaches_.end()) {
                bool now_above = (frame.mid > it->second.level_price);
                SignalKind kind = (now_above == it->second.above_level)
                    ? SignalKind::LevelRejection
                    : SignalKind::LevelBreak;
                SignalPayload payload;
                payload.dist_bps = dist_bps;
                payload.touches  = level.touches;
                Signal s {
                    .kind = kind,
                    .timestamp = frame.timestamp,
                    .ticker = ticker_,
                    .price = level.price,
                    .confidence = level.confidence,
                    .payload = payload
                };
                s.trigger_trace_id = current_trace_context().trace_id;
                bus_.publish(s);
                current_approaches_.erase(it);
            }
        }
    }
}

} // namespace trade_bot
