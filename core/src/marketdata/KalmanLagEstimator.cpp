#include "KalmanLagEstimator.hpp"
#include "logger/Logger.hpp"
#include <cmath>
#include <algorithm>

namespace trade_bot {

KalmanLagEstimator::KalmanLagEstimator() : KalmanLagEstimator(Config{}) {}

KalmanLagEstimator::KalmanLagEstimator(const Config& cfg) : cfg_(cfg) {
    reset();
}

void KalmanLagEstimator::reset() {
    x_[0] = cfg_.initial_lag_ms;
    x_[1] = cfg_.initial_drift_ms_per_sec;
    p_[0][0] = cfg_.initial_var_lag;
    p_[0][1] = 0.0;
    p_[1][0] = 0.0;
    p_[1][1] = cfg_.initial_var_drift;
}

void KalmanLagEstimator::update(double dt, double z) {
    // T4-MATH: Guard against extreme time jumps or negative dt (#140)
    if (dt <= 0.0 || dt > 3600.0) {
        if (dt > 3600.0) reset();
        return;
    }

    // ----- Predict -----
    // x = F x   with F = [[1, dt],[0,1]]
    x_[0] += dt * x_[1];

    // P = F P F^T + Q
    //   F P:
    const double fp00 = p_[0][0] + dt * p_[1][0];
    const double fp01 = p_[0][1] + dt * p_[1][1];
    const double fp10 = p_[1][0];
    const double fp11 = p_[1][1];
    //   F P F^T:
    const double pp00 = fp00 + dt * fp01;
    const double pp01 = fp01;
    const double pp10 = fp10 + dt * fp11;
    const double pp11 = fp11;
    p_[0][0] = pp00 + cfg_.q_lag   * dt;
    p_[0][1] = pp01;
    p_[1][0] = pp10;
    p_[1][1] = pp11 + cfg_.q_drift * dt;

    // ----- Update with z = lag_ms -----
    // H = [1, 0]; innovation y = z - H x ; S = H P H^T + R
    const double y_inno = z - x_[0];
    const double s      = p_[0][0] + cfg_.r_obs;
    
    // U1: log discarded outliers so they are visible in metrics/replay
    if (s < 1e-9) {
        LOG_DEBUG("KalmanLagEstimator: singular innovation variance (S={:.3e}), skipping update", s);
        return;
    }
    if (std::abs(y_inno) > cfg_.outlier_cutoff_ms) {
        LOG_DEBUG("KalmanLagEstimator: outlier lag observation {:.1f} ms, skipping update", y_inno);
        return;
    }

    // K = P H^T / S = [P00, P10]^T / S
    const double k0 = p_[0][0] / s;
    const double k1 = p_[1][0] / s;

    x_[0] += k0 * y_inno;
    x_[1] += k1 * y_inno;

    // U2: Joseph form P_new = (I-KH)*P*(I-KH)^T + K*R*K^T
    // Guarantees positive-definiteness regardless of K magnitude.
    // H=[1,0], so (I-KH) = [[1-k0,0],[-k1,1]]
    const double p00 = p_[0][0], p01 = p_[0][1], p10 = p_[1][0], p11 = p_[1][1];
    const double new00 = (1.0-k0)*(1.0-k0)*p00 + k0*k0*cfg_.r_obs;
    const double new01 = -(1.0-k0)*k1*p00 + (1.0-k0)*p01 + k0*k1*cfg_.r_obs;
    const double new10 = (1.0-k0)*(-k1*p00 + p10) + k0*k1*cfg_.r_obs;
    const double new11 = k1*k1*p00 - k1*p10 - k1*p01 + p11 + k1*k1*cfg_.r_obs;

    p_[0][0] = std::max(1e-9, new00);
    p_[1][1] = std::max(1e-9, new11);
    p_[0][1] = p_[1][0] = 0.5 * (new01 + new10);
    
    // T4-MATH: Clamp state to sane physical limits (ms and ms/sec)
    x_[0] = std::clamp(x_[0], -1000.0, 10000.0);
    x_[1] = std::clamp(x_[1], -100.0, 100.0);
}

double KalmanLagEstimator::confidence() const noexcept {
    if (cfg_.confidence_scale <= 0.0) return 0.0;
    return 1.0 / (1.0 + p_[0][0] / cfg_.confidence_scale);
}

}  // namespace trade_bot
