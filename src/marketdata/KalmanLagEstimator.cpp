#include "KalmanLagEstimator.hpp"

namespace trade_bot {

KalmanLagEstimator::KalmanLagEstimator() : KalmanLagEstimator(Config{}) {}

KalmanLagEstimator::KalmanLagEstimator(Config cfg) : cfg_(cfg) {
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
    if (s <= 0.0) return;
    // K = P H^T / S = [P00, P10]^T / S
    const double k0 = p_[0][0] / s;
    const double k1 = p_[1][0] / s;

    x_[0] += k0 * y_inno;
    x_[1] += k1 * y_inno;

    // P = (I - K H) P
    const double new00 = (1.0 - k0) * p_[0][0];
    const double new01 = (1.0 - k0) * p_[0][1];
    const double new10 = p_[1][0] - k1 * p_[0][0];
    const double new11 = p_[1][1] - k1 * p_[0][1];
    p_[0][0] = new00;
    p_[0][1] = new01;
    p_[1][0] = new10;
    p_[1][1] = new11;
}

double KalmanLagEstimator::confidence() const noexcept {
    if (cfg_.confidence_scale <= 0.0) return 0.0;
    return 1.0 / (1.0 + p_[0][0] / cfg_.confidence_scale);
}

}  // namespace trade_bot
