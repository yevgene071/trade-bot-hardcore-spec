#include "LevelDetector.hpp"
#include "logger/Logger.hpp"

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

    update_extremes_(frame);
    
    // Rebuild levels every minute
    if (frame.timestamp - last_rebuild_ >= std::chrono::minutes(1)) {
        rebuild_levels_(frame.timestamp);
        last_rebuild_ = frame.timestamp;
    }

    check_approaches_(frame);
}

void LevelDetector::rebuild() {
    rebuild_levels_(std::chrono::system_clock::now());
}

void LevelDetector::on_trade(const Trade& /*trade*/) {
}

void LevelDetector::on_book_update(const OrderBookUpdate& /*update*/) {
}

void LevelDetector::update_extremes_(const FeatureFrame& frame) {
    mid_history_.push_back({frame.timestamp, frame.mid});
    while (!mid_history_.empty() && (frame.timestamp - mid_history_.front().first) > cfg_.lookback) {
        mid_history_.pop_front();
    }

    // Multi-scale extremes: short ~1s (hw=5), medium ~5s (hw=25), long ~15s (hw=75) at 10 Hz.
    // Each scale checks a different lag point — no same-point overlap within one call.
    // Duplicates across calls (medium/long revisiting a point short already caught) are
    // rejected by binary-searching the chronologically-ordered extremes_ deque.
    static constexpr struct { size_t hw; } kScales[] = {{5}, {25}, {75}};

    for (const auto& [hw] : kScales) {
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
                auto it = std::lower_bound(extremes_.begin(), extremes_.end(), ts,
                    [](const Extreme& e, const auto& t) { return e.ts < t; });
                if (it == extremes_.end() || it->ts != ts) {
                    extremes_.insert(it, {cur, ts, is_max});
                }
            }
        }
    }

    // Fix for #145: Prune extremes history to stay within lookback
    while (!extremes_.empty() && (frame.timestamp - extremes_.front().ts) > cfg_.lookback) {
        extremes_.pop_front();
    }
}

void LevelDetector::rebuild_levels_(std::chrono::system_clock::time_point now) {
    if (extremes_.size() < cfg_.touches_min) {
        return;
    }

    std::vector<double> prices(extremes_.size());
    std::transform(extremes_.begin(), extremes_.end(), prices.begin(), [](const auto& e) { return e.price; });

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
    // We collect raw kde_val for every cluster first so we can normalize the
    // resulting kde_score against the maximum density observed THIS rebuild.
    // Replaces the previous magic constant (kde_val * 100.0), which produced
    // either always-saturated or always-near-zero scores depending on the
    // ticker's absolute volume scale.  Fixes #117.
    struct Pending {
        double                                centroid;
        std::size_t                           cluster_size;
        double                                kde_val;
        std::chrono::system_clock::time_point last_ts;
    };
    // Precompute sorted (price, ts) for O(logE) range queries inside the cluster loop
    std::vector<std::pair<double, std::chrono::system_clock::time_point>> sorted_extremes;
    sorted_extremes.reserve(extremes_.size());
    for (const auto& e : extremes_) sorted_extremes.emplace_back(e.price, e.ts);
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

    active_levels_ = std::move(new_levels);
}

void LevelDetector::check_approaches_(const FeatureFrame& frame) {
    const double price_inc = book_.price_increment();
    for (const auto& level : active_levels_) {
        double dist_bps = (frame.mid - level.price) / frame.mid * 10000.0;
        const auto tick = PriceTick::from_price(level.price, price_inc);
        
        if (std::abs(dist_bps) <= cfg_.approach_trigger_bps) {
            if (current_approaches_.find(tick) == current_approaches_.end()) {
                Signal s {
                    .kind = SignalKind::LevelApproach,
                    .timestamp = frame.timestamp,
                    .ticker = ticker_,
                    .price = level.price,
                    .confidence = level.confidence,
                    .payload = nlohmann::json{
                        {"dist_bps", dist_bps},
                        {"touches", level.touches}
                    }
                };
                bus_.publish(s);
                current_approaches_[tick] = {level.price, frame.timestamp};
            }
        } else {
            current_approaches_.erase(tick);
        }
    }
}

} // namespace trade_bot
