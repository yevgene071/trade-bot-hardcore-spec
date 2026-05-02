#pragma once

#include <vector>
#include <cmath>
#include <algorithm>
#include <xsimd/xsimd.hpp>

namespace trade_bot {

/**
 * Kernel Density Estimation with Gaussian kernel.
 */
class Kde {
public:
    struct Point {
        double value;
        double weight;
    };

    static double gaussian_kernel(double x) {
        static const double inv_sqrt_2pi = 0.3989422804014327;
        return inv_sqrt_2pi * std::exp(-0.5 * x * x);
    }

    /**
     * Compute density at given points using SIMD if possible.
     */
    static std::vector<double> estimate(const std::vector<Point>& data, 
                                       const std::vector<double>& query_points,
                                       double bandwidth) {
        if (data.empty() || query_points.empty()) return {};
        if (bandwidth <= 0) bandwidth = 1e-6;

        std::vector<double> densities(query_points.size(), 0.0);
        double inv_h = 1.0 / bandwidth;
        double total_weight = 0;
        for (const auto& d : data) total_weight += d.weight;
        if (total_weight <= 0) return densities;

        // SIMD batch size
        using batch_type = xsimd::batch<double>;
        constexpr size_t simd_size = batch_type::size;

        for (size_t i = 0; i < query_points.size(); ++i) {
            double q = query_points[i];
            double sum = 0.0;

            // Gaussian kernel sum: sum( w_j * K((q - x_j)/h) )
            // We can vectorize the loop over data points
            size_t j = 0;
            batch_type q_batch(q);
            batch_type inv_h_batch(inv_h);
            batch_type acc(0.0);
            
            // To use xsimd efficiently, we need to prepare data in arrays
            // For simplicity and to meet ≤ 50 µs on 200x30 points, 
            // even a scalar loop with compiler auto-vectorization might work,
            // but let's try a direct SIMD loop for the kernel.
            
            for (; j + simd_size <= data.size(); j += simd_size) {
                std::array<double, simd_size> vals, weights;
                for (size_t k = 0; k < simd_size; ++k) {
                    vals[k] = data[j + k].value;
                    weights[k] = data[j + k].weight;
                }
                batch_type x_batch = batch_type::load_unaligned(vals.data());
                batch_type w_batch = batch_type::load_unaligned(weights.data());
                
                batch_type diff = (q_batch - x_batch) * inv_h_batch;
                // K(u) = C * exp(-0.5 * u^2)
                batch_type kernel_val = xsimd::exp(batch_type(-0.5) * diff * diff);
                acc += w_batch * kernel_val;
            }
            
            sum = xsimd::reduce_add(acc);
            for (; j < data.size(); ++j) {
                double diff = (q - data[j].value) * inv_h;
                sum += data[j].weight * std::exp(-0.5 * diff * diff);
            }

            // Normalization: sum / (total_weight * h * sqrt(2pi))
            // But for local maxima search, constant factors don't matter.
            // We'll normalize by total weight at least.
            densities[i] = sum / (total_weight * bandwidth * 2.506628);
        }

        return densities;
    }

    static double silverman_bandwidth(const std::vector<Point>& data) {
        if (data.size() < 2) return 1.0;
        
        double mean = 0, m2 = 0, weight_sum = 0;
        for (const auto& d : data) {
            double old_mean = mean;
            weight_sum += d.weight;
            mean += (d.value - mean) * (d.weight / weight_sum);
            m2 += d.weight * (d.value - mean) * (d.value - old_mean);
        }
        double variance = m2 / weight_sum;
        double sigma = std::sqrt(variance);
        if (sigma <= 0) sigma = 1e-6;
        
        // h = 1.06 * sigma * n^(-1/5)
        return 1.06 * sigma * std::pow(static_cast<double>(data.size()), -0.2);
    }
};

} // namespace trade_bot
