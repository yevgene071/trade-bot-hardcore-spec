#pragma once

#include <vector>
#include <algorithm>
#include <cmath>
#include <numeric>

namespace trade_bot {

/**
 * 1D DBSCAN for price clustering.
 * Optimized for O(N log N) using sorting and sliding window for neighbor search.
 */
class Dbscan1D {
public:
    struct Point {
        double value;
        int    cluster_id{-1}; // -1 for noise
    };

    static std::vector<std::vector<double>> cluster(const std::vector<double>& values, 
                                                   double eps, 
                                                   size_t min_pts) {
        if (values.empty()) return {};
        
        std::vector<Point> points(values.size());
        std::transform(values.begin(), values.end(), points.begin(), [](double v) {
            return Point{v, -1};
        });
        
        // Sort for optimized neighbor search
        std::sort(points.begin(), points.end(), [](const Point& a, const Point& b){
            return a.value < b.value;
        });

        // Use a generation counter instead of O(N) fill to track queue membership
        std::vector<int> visited_gen(points.size(), 0);
        int current_gen = 0;

        int next_cluster_id = 0;
        for (size_t i = 0; i < points.size(); ++i) {
            if (points[i].cluster_id != -1) continue;

            auto range = find_range_(points, points[i].value, eps);
            if (static_cast<size_t>(std::distance(range.first, range.second)) < min_pts) {
                continue;
            }

            int cur_cluster_id = next_cluster_id++;
            points[i].cluster_id = cur_cluster_id;

            std::vector<size_t> queue;
            queue.reserve(points.size());
            current_gen++; // New generation for this cluster expansion

            for (auto it = range.first; it != range.second; ++it) {
                size_t idx = static_cast<size_t>(it - points.begin());
                if (idx != i) {
                    queue.push_back(idx);
                    visited_gen[idx] = current_gen;
                }
            }

            for (size_t k = 0; k < queue.size(); ++k) {
                size_t q_idx = queue[k];
                if (points[q_idx].cluster_id == -1) {
                    points[q_idx].cluster_id = cur_cluster_id;

                    auto q_range = find_range_(points, points[q_idx].value, eps);
                    if (static_cast<size_t>(std::distance(q_range.first, q_range.second)) >= min_pts) {
                        for (auto it = q_range.first; it != q_range.second; ++it) {
                            size_t n_idx = static_cast<size_t>(it - points.begin());
                            if (points[n_idx].cluster_id == -1 && visited_gen[n_idx] != current_gen) {
                                queue.push_back(n_idx);
                                visited_gen[n_idx] = current_gen;
                            }
                        }
                    }
                }
            }
        }

        // Single O(N) pass instead of O(N*K) nested loop
        std::vector<std::vector<double>> result(static_cast<size_t>(next_cluster_id));
        for (const auto& p : points) {
            if (p.cluster_id >= 0) {
                result[static_cast<size_t>(p.cluster_id)].push_back(p.value);
            }
        }
        return result;
    }

private:
    using PointIter = std::vector<Point>::const_iterator;
    
    static std::pair<PointIter, PointIter> find_range_(const std::vector<Point>& points, double val, double eps) {
        auto lo = std::lower_bound(points.begin(), points.end(), val - eps, [](const Point& p, double v) {
            return p.value < v;
        });
        auto hi = std::upper_bound(lo, points.end(), val + eps, [](double v, const Point& p) {
            return v < p.value;
        });
        return {lo, hi};
    }
};

} // namespace trade_bot
