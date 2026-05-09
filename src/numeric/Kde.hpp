#pragma once

#include <vector>
#include <cmath>
#include <algorithm>
#include <xsimd/xsimd.hpp>
#include "Hmm.hpp"
#include "Welford.hpp"
#include <numeric>

namespace trade_bot {

/**
 * Kernel Density Estimation with Gaussian kernel.
 * Optimized with SIMD and SoA (Structure of Arrays) layout.
 */
class Kde {
public:
    struct DataSet {
        std::vector<double> values;
        std::vector<double> weights;
        double total_weight{0.0};
    };

    static double gaussian_kernel(double x) {
        return kInvSqrt2Pi * std::exp(-0.5 * x * x);
    }

    /**
     * Compute density at given points using SIMD with SoA data.
     */
    static std::vector<double> estimate(const DataSet& data, 
                                       const std::vector<double>& query_points,
                                       double bandwidth) {
        if (data.values.empty() || query_points.empty()) return {};
        if (bandwidth <= 0) bandwidth = 1e-6;

        std::vector<double> densities(query_points.size(), 0.0);
        double inv_h = 1.0 / bandwidth;
        if (data.total_weight <= 0) return densities;

        // SIMD batch size
        using batch_type = xsimd::batch<double>;
        constexpr size_t simd_size = batch_type::size;

        for (size_t i = 0; i < query_points.size(); ++i) {
            double q = query_points[i];
            
            size_t j = 0;
            batch_type q_batch(q);
            batch_type inv_h_batch(inv_h);
            batch_type acc(0.0);
            
            // SoA allows direct unaligned loads without stack gather
            for (; j + simd_size <= data.values.size(); j += simd_size) {
                batch_type x_batch = batch_type::load_unaligned(&data.values[j]);
                batch_type w_batch = batch_type::load_unaligned(&data.weights[j]);
                
                batch_type diff = (q_batch - x_batch) * inv_h_batch;
                batch_type kernel_val = xsimd::exp(batch_type(-0.5) * diff * diff);
                acc += w_batch * kernel_val;
            }
            
            double sum = xsimd::reduce_add(acc);
            for (; j < data.values.size(); ++j) {
                double diff = (q - data.values[j]) * inv_h;
                sum += data.weights[j] * std::exp(-0.5 * diff * diff);
            }

            densities[i] = sum / (data.total_weight * bandwidth * kSqrt2Pi);
        }

        return densities;
    }

    static double silverman_bandwidth(const DataSet& data) {
        if (data.values.size() < 2) return 1.0;

        WeightedWelfordAccumulator<double> acc;
        size_t idx = 0;
        std::for_each(data.values.begin(), data.values.end(),
                      [&](double v) { acc.update(v, data.weights[idx++]); });

        double sigma = acc.stdev();
        if (sigma <= 0) sigma = 1e-6;
        
        return 1.06 * sigma * std::pow(static_cast<double>(data.values.size()), -0.2);
    }
};

} // namespace trade_bot
