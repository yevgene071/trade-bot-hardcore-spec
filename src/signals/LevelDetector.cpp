#include "LevelDetector.hpp"
#include "logger/Logger.hpp"

#include <cmath>
#include <algorithm>
#include <iostream>

namespace trade_bot {

LevelDetector::LevelDetector(Ticker ticker,
                           SignalBus& bus,
                           const OrderBook& book,
                           const ClusterSnapshotManager& cluster_mgr,
                           Config cfg)
    : ticker_(std::move(ticker))
    , bus_(bus)
    , book_(book)
    , cluster_mgr_(cluster_mgr)
    , cfg_(cfg) {}

LevelDetector::LevelDetector(Ticker ticker,
                           SignalBus& bus,
                           const OrderBook& book,
                           const ClusterSnapshotManager& cluster_mgr)
    : LevelDetector(std::move(ticker), bus, book, cluster_mgr, Config{}) {}

void LevelDetector::on_frame(const FeatureFrame& frame) {
    if (frame.ticker != ticker_) return;

    update_extremes_(frame);
    
    // Rebuild levels every minute
    static auto last_rebuild = std::chrono::system_clock::time_point{};
    if (frame.timestamp - last_rebuild >= std::chrono::minutes(1)) {
        rebuild_levels_(frame.timestamp);
        last_rebuild = frame.timestamp;
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

    if (mid_history_.size() < 11) return;

    size_t i = mid_history_.size() - 6; 

    double cur = mid_history_[i].second;
    bool is_max = true, is_min = true;
    for (size_t j = i - 5; j <= i + 5; ++j) {
        if (j == i) continue;
        if (mid_history_[j].second >= cur) is_max = false;
        if (mid_history_[j].second <= cur) is_min = false;
    }

    if (is_max || is_min) {
        double start = mid_history_[i-5].second;
        double end = mid_history_[i+5].second;
        double magnitude_bps = std::max(std::abs(cur - start), std::abs(cur - end)) / cur * 10000.0;
        
        if (magnitude_bps >= cfg_.min_reversal_bps) {
            extremes_.push_back({cur, mid_history_[i].first, is_max});
        }
    }
}

void LevelDetector::rebuild_levels_(std::chrono::system_clock::time_point now) {
    if (extremes_.size() < cfg_.touches_min) {
        return;
    }

    std::vector<double> prices;
    for (const auto& e : extremes_) prices.push_back(e.price);

    double mid = book_.mid().value_or(0.0);
    if (mid == 0.0 && !mid_history_.empty()) mid = mid_history_.back().second;
    if (mid == 0.0) return;
    
    double eps = mid * (cfg_.cluster_tolerance_bps / 10000.0);

    auto clusters = Dbscan1D::cluster(prices, eps, cfg_.touches_min);
    
    std::vector<Level> new_levels;
    
    std::vector<Kde::Point> kde_points;
    auto m15 = cluster_mgr_.get(ticker_, "M15");
    if (m15) {
        for (const auto& item : m15->items) {
            kde_points.push_back({item.price, item.ask_size + item.bid_size});
        }
    }

    double h = Kde::silverman_bandwidth(kde_points) * cfg_.kde_smoothness;

    for (const auto& c : clusters) {
        double sum = 0;
        for (double p : c) sum += p;
        double centroid = sum / c.size();

        double kde_val = 0.0;
        if (!kde_points.empty()) {
            auto dens = Kde::estimate(kde_points, {centroid}, h);
            kde_val = dens[0];
        }

        auto last_ts = std::chrono::system_clock::time_point{};
        for (const auto& e : extremes_) {
            if (std::abs(e.price - centroid) <= eps) {
                if (e.ts > last_ts) last_ts = e.ts;
            }
        }

        double age_min = std::chrono::duration_cast<std::chrono::minutes>(now - last_ts).count();
        double recency_score = std::exp(-age_min / 60.0);
        double touches_score = std::min(1.0, static_cast<double>(c.size()) / 5.0);
        double kde_score = std::min(1.0, kde_val * 100.0); 

        double confidence = 0.4 * touches_score + 0.4 * kde_score + 0.2 * recency_score;

        new_levels.push_back(Level{
            .price = centroid,
            .touches = static_cast<int>(c.size()),
            .kde_score = kde_score,
            .last_touch = last_ts,
            .created_at = now,
            .source = "extreme",
            .confidence = confidence
        });
    }

    active_levels_ = std::move(new_levels);
}

void LevelDetector::check_approaches_(const FeatureFrame& frame) {
    for (const auto& level : active_levels_) {
        double dist_bps = (frame.mid - level.price) / frame.mid * 10000.0;
        
        if (std::abs(dist_bps) <= cfg_.approach_trigger_bps) {
            if (current_approaches_.find(level.price) == current_approaches_.end()) {
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
                current_approaches_[level.price] = {level.price, frame.timestamp};
            }
        } else {
            current_approaches_.erase(level.price);
        }
    }
}

} // namespace trade_bot
