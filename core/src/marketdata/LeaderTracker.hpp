#pragma once

#include "KalmanLagEstimator.hpp"
#include "domain/Types.hpp"
#include "numeric/WelfordCorrelation.hpp"

#include <cstddef>
#include <deque>
#include <vector>

namespace trade_bot {

/**
 * T1-LEADER: tracks leader↔follower correlation and lag for a single pair.
 *
 * On every paired observation it:
 *   - updates an online Pearson correlation (Welford / Schubert & Gertz 2018)
 *   - feeds a 2-state Kalman lag estimator with the latest argmax cross-corr
 *     observation (computed across a sliding 10-second window, stride 100 ms)
 *   - runs a CUSUM monitor on the correlation series to fire an early
 *     decorrelation alarm before naive `corr < threshold` would.
 *
 * Inputs are pre-aligned mid-price samples for both legs at a fixed cadence
 * (caller-controlled — e.g. 100 ms ticks). The class is single-threaded and
 * allocation-light: only the rolling window grows up to its configured size.
 */
class LeaderTracker {
public:
    struct Config {
        std::size_t                xcorr_window{100};      // 10 s @ 100 ms
        std::size_t                xcorr_max_lag_steps{10}; // ±1 s @ 100 ms
        double                     dt_sec{0.1};            // sample period
        // CUSUM (Page 1954) on correlation drift.
        std::size_t                cusum_warmup{30};       // skip noisy startup
        double                     cusum_baseline{0.7};    // expected corr
        double                     cusum_drift{0.05};      // slack k
        double                     cusum_threshold{1.0};   // alarm at h
        // Kalman tuning passed straight through.
        KalmanLagEstimator::Config kalman{};
    };

    LeaderTracker();
    explicit LeaderTracker(Config cfg);

    /// Push a paired sample (leader_price, follower_price) at the configured
    /// dt. The caller is expected to align timestamps externally.
    void update(double leader_price, double follower_price);

    /// Online Pearson correlation (Welford), uses all samples since reset.
    double correlation() const;

    /// Kalman-smoothed lag of the follower behind the leader (ms).
    double lag_ms() const;

    /// Kalman covariance-based confidence in [0, 1].
    double confidence() const;

    /// CUSUM has fired since the last reset_cusum().
    bool decorrelation_alarm() const noexcept { return cusum_alarm_; }
    void reset_cusum() noexcept;

    /// Number of samples observed.
    std::size_t sample_count() const noexcept { return n_; }

private:
    /// Recompute argmax cross-correlation observation from the window and
    /// feed it into the Kalman filter. Returns the chosen lag (in ms).
    double recompute_lag_observation_();

    Config                          cfg_;
    WelfordCorrelation<double>      corr_;
    std::deque<double>              w_leader_;
    std::deque<double>              w_follower_;
    KalmanLagEstimator              kalman_;
    std::size_t                     n_{0};

    // CUSUM state (one-sided, alarms when correlation drops below baseline).
    double                          cusum_minus_{0.0};
    bool                            cusum_alarm_{false};

    // T1: reuse scratch buffers across recompute_lag_observation_ calls
    mutable std::vector<double>     scratch_dl_;
    mutable std::vector<double>     scratch_df_;
};

}  // namespace trade_bot
