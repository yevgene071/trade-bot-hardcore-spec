#pragma once

#include <vector>
#include <array>
#include <cmath>

namespace trade_bot {

/**
 * 3-state HMM implementation for approach classification.
 * States: 0=Impulse, 1=Slow, 2=Consolidation
 */
class ApproachHmm {
public:
    static constexpr size_t kStates = 3;

    struct Params {
        std::array<double, kStates> start_probs;
        std::array<std::array<double, kStates>, kStates> trans_matrix;
        
        // Emission params (Gaussian for simplicity: mean + std)
        // Observations: [speed, pullbacks, dist]
        struct Emission {
            std::array<double, 3> means;
            std::array<double, 3> stds;
        };
        std::array<Emission, kStates> emissions;
    };

    explicit ApproachHmm(Params params) : params_(std::move(params)) {}

    /**
     * Forward algorithm to compute state probabilities.
     * obs: vector of [speed, pullbacks, dist]
     */
    std::array<double, kStates> predict(const std::vector<std::array<double, 3>>& observations) const {
        if (observations.empty()) return params_.start_probs;

        std::array<double, kStates> alpha;
        
        // Initial step
        for (size_t s = 0; s < kStates; ++s) {
            alpha[s] = params_.start_probs[s] * emission_prob_(s, observations[0]);
        }
        double sum = alpha[0] + alpha[1] + alpha[2];
        if (sum > 0) {
            double inv_sum = 1.0 / sum;
            alpha[0] *= inv_sum; alpha[1] *= inv_sum; alpha[2] *= inv_sum;
        }

        // Recursive steps
        for (size_t t = 1; t < observations.size(); ++t) {
            std::array<double, kStates> next_alpha;
            // Manual unroll for kStates=3 to avoid inner_product/lambda overhead
            for (size_t j = 0; j < kStates; ++j) {
                double trans_sum = alpha[0] * params_.trans_matrix[0][j] +
                                 alpha[1] * params_.trans_matrix[1][j] +
                                 alpha[2] * params_.trans_matrix[2][j];
                next_alpha[j] = trans_sum * emission_prob_(j, observations[t]);
            }
            double next_sum = next_alpha[0] + next_alpha[1] + next_alpha[2];
            if (next_sum > 0) {
                double inv_sum = 1.0 / next_sum;
                alpha[0] = next_alpha[0] * inv_sum;
                alpha[1] = next_alpha[1] * inv_sum;
                alpha[2] = next_alpha[2] * inv_sum;
            }
        }

        return alpha;
    }

private:
    double emission_prob_(size_t state, const std::array<double, 3>& obs) const {
        const auto& e = params_.emissions[state];
        double p = 1.0;
        for (size_t i = 0; i < 3; ++i) {
            double diff = obs[i] - e.means[i];
            double var = std::max(e.stds[i] * e.stds[i], 1e-6);
            p *= (1.0 / (e.stds[i] * 2.5066)) * std::exp(-0.5 * diff * diff / var);
        }
        return std::max(p, 1e-10);
    }

    Params params_;
};

} // namespace trade_bot
