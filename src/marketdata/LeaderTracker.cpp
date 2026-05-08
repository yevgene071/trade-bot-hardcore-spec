#include "LeaderTracker.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace trade_bot {

LeaderTracker::LeaderTracker() : LeaderTracker(Config{}) {}

LeaderTracker::LeaderTracker(Config cfg)
    : cfg_(cfg)
    , kalman_(cfg.kalman) {}

void LeaderTracker::update(double leader_price, double follower_price) {
    corr_.update(leader_price, follower_price);
    ++n_;

    if (w_leader_.size() == cfg_.xcorr_window) {
        w_leader_.pop_front();
        w_follower_.pop_front();
    }
    w_leader_.push_back(leader_price);
    w_follower_.push_back(follower_price);

    // Feed the Kalman filter with the latest cross-corr argmax observation.
    if (w_leader_.size() >= 2 * cfg_.xcorr_max_lag_steps + 4) {
        const double obs_lag_ms = recompute_lag_observation_();
        kalman_.update(cfg_.dt_sec, obs_lag_ms);
    }

    // CUSUM on correlation: alarm when (baseline - corr) accumulates above h.
    // Warmup: small-n correlation is meaningless (≈±1 from a few points), so
    // skip the first `cusum_warmup` samples entirely.
    if (n_ > cfg_.cusum_warmup) {
        const double c = correlation();
        cusum_minus_ =
            std::max(0.0, cusum_minus_ + (cfg_.cusum_baseline - c - cfg_.cusum_drift));
        if (cusum_minus_ > cfg_.cusum_threshold) {
            cusum_alarm_ = true;
        }
    }
}

double LeaderTracker::correlation() const {
    return corr_.correlation();
}

double LeaderTracker::lag_ms() const {
    return kalman_.lag_ms();
}

double LeaderTracker::confidence() const {
    return kalman_.confidence();
}

void LeaderTracker::reset_cusum() noexcept {
    cusum_minus_ = 0.0;
    cusum_alarm_ = false;
}

double LeaderTracker::recompute_lag_observation_() {
    // Direct argmax cross-correlation over [-K..+K] sample shifts. K small
    // (default 10) so this is O(K * W) — well below 100 µs at warmup sizes.
    const std::size_t W = w_leader_.size();
    const auto K = static_cast<int>(cfg_.xcorr_max_lag_steps);

    auto mean_of = [&](const std::deque<double>& v) {
        return std::accumulate(v.begin(), v.end(), 0.0) / static_cast<double>(v.size());
    };
    const double mean_l = mean_of(w_leader_);
    const double mean_f = mean_of(w_follower_);

    double best_corr = -2.0;
    int    best_shift = 0;

    for (int shift = -K; shift <= K; ++shift) {
        // shift > 0 means follower trails leader by `shift` samples.
        const std::size_t lo = static_cast<std::size_t>(std::max(0, shift));
        const std::size_t hi = W - static_cast<std::size_t>(std::max(0, -shift));
        if (lo + 4 >= hi) continue;

        double sxy = 0.0, sxx = 0.0, syy = 0.0;
        for (std::size_t i = lo; i < hi; ++i) {
            const double dx = w_leader_[i - static_cast<std::size_t>(std::max(0, shift))]
                              - mean_l;
            const double dy = w_follower_[i] - mean_f;
            sxy += dx * dy;
            sxx += dx * dx;
            syy += dy * dy;
        }
        if (sxx <= 0.0 || syy <= 0.0) continue;
        const double r = sxy / std::sqrt(sxx * syy);
        if (r > best_corr) {
            best_corr  = r;
            best_shift = shift;
        }
    }

    return static_cast<double>(best_shift) * cfg_.dt_sec * 1000.0;
}

}  // namespace trade_bot
