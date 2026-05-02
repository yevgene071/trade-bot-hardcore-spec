#pragma once

#include <vector>
#include <algorithm>

namespace trade_bot {

/**
 * 1D DBSCAN for price clustering.
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
        
        std::vector<Point> points;
        points.reserve(values.size());
        for (double v : values) points.push_back({v});
        
        // Sort to make neighbor search O(1) or O(log N)
        std::sort(points.begin(), points.end(), [](const Point& a, const Point& b){
            return a.value < b.value;
        });

        int next_cluster_id = 0;
        for (size_t i = 0; i < points.size(); ++i) {
            if (points[i].cluster_id != -1) continue;

            // Find neighbors
            std::vector<size_t> neighbors;
            for (size_t j = 0; j < points.size(); ++j) {
                if (std::abs(points[i].value - points[j].value) <= eps) {
                    neighbors.push_back(j);
                }
            }

            if (neighbors.size() < min_pts) {
                // Noise for now
                continue;
            }

            // Start a new cluster
            int cur_cluster_id = next_cluster_id++;
            points[i].cluster_id = cur_cluster_id;

            // Expand cluster
            std::vector<size_t> queue = neighbors;
            for (size_t k = 0; k < queue.size(); ++k) {
                size_t q_idx = queue[k];
                if (points[q_idx].cluster_id == -1) {
                    points[q_idx].cluster_id = cur_cluster_id;
                    
                    // Find neighbors of neighbor
                    std::vector<size_t> q_neighbors;
                    for (size_t j = 0; j < points.size(); ++j) {
                        if (std::abs(points[q_idx].value - points[j].value) <= eps) {
                            q_neighbors.push_back(j);
                        }
                    }
                    if (q_neighbors.size() >= min_pts) {
                        for (size_t n : q_neighbors) {
                            if (std::find(queue.begin(), queue.end(), n) == queue.end()) {
                                queue.push_back(n);
                            }
                        }
                    }
                }
            }
        }

        // Group points by cluster_id
        std::vector<std::vector<double>> result;
        for (int id = 0; id < next_cluster_id; ++id) {
            std::vector<double> group;
            for (const auto& p : points) {
                if (p.cluster_id == id) group.push_back(p.value);
            }
            result.push_back(std::move(group));
        }
        return result;
    }
};

} // namespace trade_bot
