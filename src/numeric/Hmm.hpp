#pragma once

#include <vector>
#include <array>
#include <cmath>

namespace trade_bot {

inline constexpr double kSqrt2Pi    = 2.5066282746310002;
inline constexpr double kInvSqrt2Pi = 0.3989422804014327; // 1 / sqrt(2π)

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

    explicit ApproachHmm(Params params) : params_(std::move(params)) {
        // T4-MATH: Precompute log-space constants to avoid exp() in inner loop (#155)
        for (size_t s = 0; s < kStates; ++s) {
            log_start_probs_[s] = std::log(std::max(params_.start_probs[s], 1e-10));
            for (size_t j = 0; j < kStates; ++j) {
                log_trans_matrix_[s][j] = std::log(std::max(params_.trans_matrix[s][j], 1e-10));
            }
            for (size_t i = 0; i < 3; ++i) {
                double sigma = std::max(params_.emissions[s].stds[i], 1e-9);
                log_inv_norm_[s][i] = std::log(1.0 / (sigma * kSqrt2Pi));
                inv_var_[s][i]      = 1.0 / (2.0 * sigma * sigma);
            }
        }
    }

    std::array<double, kStates> predict(const std::vector<std::array<double, 3>>& observations) const {
        if (observations.empty()) return params_.start_probs;

        std::array<double, kStates> log_alpha;
        
        // Initial step
        for (size_t s = 0; s < kStates; ++s) {
            log_alpha[s] = log_start_probs_[s] + log_emission_prob_(s, observations[0]);
        }

        // Recursive steps
        for (size_t t = 1; t < observations.size(); ++t) {
            std::array<double, kStates> next_log_alpha;
            for (size_t j = 0; j < kStates; ++j) {
                // Log-sum-exp for transition
                double max_a = -1e100;
                for (size_t i = 0; i < kStates; ++i) {
                    max_a = std::max(max_a, log_alpha[i] + log_trans_matrix_[i][j]);
                }
                double sum_exp = 0.0;
                for (size_t i = 0; i < kStates; ++i) {
                    sum_exp += std::exp(log_alpha[i] + log_trans_matrix_[i][j] - max_a);
                }
                next_log_alpha[j] = max_a + std::log(sum_exp) + log_emission_prob_(j, observations[t]);
            }
            log_alpha = next_log_alpha;
            
            // Normalize log_alpha to prevent drift
            double max_val = *std::max_element(log_alpha.begin(), log_alpha.end());
            for (double& a : log_alpha) a -= max_val;
        }

        // Convert back to probabilities
        std::array<double, kStates> probs;
        double sum_exp = 0.0;
        for (size_t s = 0; s < kStates; ++s) {
            probs[s] = std::exp(log_alpha[s]);
            sum_exp += probs[s];
        }
        if (sum_exp > 0) {
            for (double& p : probs) p /= sum_exp;
        }
        return probs;
    }

private:
    double log_emission_prob_(size_t state, const std::array<double, 3>& obs) const {
        double lp = 0.0;
        for (size_t i = 0; i < 3; ++i) {
            double diff = obs[i] - params_.emissions[state].means[i];
            lp += log_inv_norm_[state][i] - (diff * diff * inv_var_[state][i]);
        }
        return lp;
    }

    Params params_;
    std::array<double, kStates> log_start_probs_{};
    std::array<std::array<double, kStates>, kStates> log_trans_matrix_{};
    std::array<std::array<double, 3>, kStates> log_inv_norm_{};
    std::array<std::array<double, 3>, kStates> inv_var_{};
};

} // namespace trade_bot
