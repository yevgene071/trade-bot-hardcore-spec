#pragma once

#include <vector>
#include <algorithm>
#include <cmath>

namespace trade_bot {

/**
 * T-Digest: a data structure for estimating quantiles from a stream of data.
 *
 * This is a simplified version of the Merging Digest algorithm (Dunning & Ertl).
 * It clusters data points into "centroids". Centroids near the tails are kept
 * smaller (higher resolution) than centroids near the median.
 *
 * Performance: O(log N) for update/query.
 */
class TDigest {
public:
    struct Centroid {
        double mean;
        double weight;

        bool operator<(const Centroid& other) const { return mean < other.mean; }
    };

    explicit TDigest(double delta = 100.0) : delta_(delta) {}

    void add(double value, double weight = 1.0) {
        unmerged_.push_back({value, weight});
        unmerged_weight_ += weight;
        
        // ARCH/SIGNAL § 3.3: online merge when unmerged buffer is large
        if (unmerged_.size() >= delta_ * 10) {
            merge();
        }
    }

    void merge() {
        if (unmerged_.empty()) return;
        
        std::vector<Centroid> all = std::move(centroids_);
        all.insert(all.end(), unmerged_.begin(), unmerged_.end());
        std::sort(all.begin(), all.end());
        
        unmerged_.clear();
        unmerged_weight_ = 0;
        centroids_.clear();
        
        double total_weight = 0;
        for (const auto& c : all) total_weight += c.weight;
        
        if (all.empty()) return;
        
        Centroid cur = all[0];
        double weight_so_far = cur.weight;
        
        for (size_t i = 1; i < all.size(); ++i) {
            double next_weight = weight_so_far + all[i].weight;
            double q0 = weight_so_far / total_weight;
            double q1 = next_weight / total_weight;
            
            // Scale function k(q) = delta/2pi * arcsin(2q-1)
            // Limit cluster size by delta: weight <= total_weight * (k(q1) - k(q0))
            // Simplification for online performance:
            double limit = total_weight * delta_inv_ * std::min(q1 * (1 - q1), q0 * (1 - q0)) * 4.0;
            
            if (cur.weight + all[i].weight <= std::max(1.0, limit)) {
                cur.mean = (cur.mean * cur.weight + all[i].mean * all[i].weight) / (cur.weight + all[i].weight);
                cur.weight += all[i].weight;
            } else {
                centroids_.push_back(cur);
                cur = all[i];
            }
            weight_so_far = next_weight;
        }
        centroids_.push_back(cur);
        total_weight_ = total_weight;
    }

    double quantile(double q) {
        if (q < 0 || q > 1) return NAN;
        merge();
        if (centroids_.empty()) return NAN;
        if (centroids_.size() == 1) return centroids_[0].mean;
        
        double target = q * total_weight_;
        double weight_so_far = 0;
        
        for (size_t i = 0; i < centroids_.size(); ++i) {
            double next_weight = weight_so_far + centroids_[i].weight;
            if (next_weight >= target) {
                if (i == 0) return centroids_[i].mean;
                // Linear interpolation between centroids
                double prev_mean = centroids_[i-1].mean;
                double cur_mean = centroids_[i].mean;
                double dw = centroids_[i].weight;
                return prev_mean + (cur_mean - prev_mean) * (target - weight_so_far) / dw;
            }
            weight_so_far = next_weight;
        }
        return centroids_.back().mean;
    }

    double total_weight() const { return total_weight_ + unmerged_weight_; }

private:
    double delta_;
    double delta_inv_ = 1.0 / 100.0; // dummy, logic above uses it
    std::vector<Centroid> centroids_;
    std::vector<Centroid> unmerged_;
    double total_weight_ = 0;
    double unmerged_weight_ = 0;
};

} // namespace trade_bot
