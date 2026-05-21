#pragma once

#include <cstdint>

namespace trade_bot {

/**
 * 2-state Kalman filter that tracks the lead/lag (ms) between a leader and a
 * follower instrument and the rate at which that lag drifts (ms/s).
 *
 * State:           x = [lag_ms, drift_ms_per_sec]^T
 * Transition (dt): F = [[1, dt], [0, 1]]
 * Observation:     z = lag_ms,  H = [1, 0]
 *
 * Q (process noise) and R (observation variance) are configurable. Confidence
 * for downstream consumers (LeaderMove.confidence) is derived from the
 * lag-axis posterior variance P00:
 *   confidence = 1 / (1 + P00 / confidence_scale)
 *
 * The implementation is allocation-free and noexcept-safe — covariance and
 * gain math is unrolled for the 2×2 case.
 */
class KalmanLagEstimator {
public:
    struct Config {
        double initial_lag_ms{0.0};
        double initial_drift_ms_per_sec{0.0};
        double initial_var_lag{1e6};        // very loose prior — first obs anchors
        double initial_var_drift{1e3};
        double q_lag{1.0};                  // process noise on lag (ms²/s)
        double q_drift{0.01};                // process noise on drift ((ms/s)²/s)
        double r_obs{2500.0};                // observation variance (ms²) ≈ σ=50 ms
        double confidence_scale{2500.0};
        double outlier_cutoff_ms{5000.0};    // U1: move hardcoded outlier cutoff to config
    };

    KalmanLagEstimator();
    explicit KalmanLagEstimator(const Config& cfg);

    /// Advance state by `dt_sec` and incorporate observed lag in ms.
    void update(double dt_sec, double observed_lag_ms);

    double lag_ms()        const noexcept { return x_[0]; }
    double drift_ms_per_s() const noexcept { return x_[1]; }
    double variance_lag()  const noexcept { return p_[0][0]; }
    double confidence()    const noexcept;

    void reset();

private:
    Config cfg_;
    double x_[2]{};      // state
    double p_[2][2]{};   // covariance
};

}  // namespace trade_bot
