#include "FeatureExtractor.hpp"

#include <algorithm>
#include <cmath>

namespace trade_bot {

namespace {
constexpr double kBpsScale = 10'000.0;
}

FeatureExtractor::FeatureExtractor(Ticker ticker)
    : FeatureExtractor(std::move(ticker), Config{}) {}

FeatureExtractor::FeatureExtractor(Ticker ticker, Config cfg)
    : ticker_(std::move(ticker))
    , cfg_(cfg)
    , hist_book_(cfg.latency_max_us, cfg.hdr_significant_digits)
    , hist_total_(cfg.latency_max_us, cfg.hdr_significant_digits) {}

void FeatureExtractor::set_sources(const OrderBook* ob,
                                   const TradeStream* ts,
                                   const LeaderTracker* lt) {
    ob_ = ob;
    ts_ = ts;
    lt_ = lt;
}

double FeatureExtractor::price_change_pct_(std::chrono::system_clock::time_point now,
                                           std::chrono::seconds horizon) const {
    if (mid_history_.empty()) return 0.0;
    const auto cutoff = now - horizon;
    auto it = std::find_if(mid_history_.begin(), mid_history_.end(), [&](const auto& s) {
        return s.t >= cutoff;
    });
    double ref_mid = (it != mid_history_.end()) ? it->mid : mid_history_.front().mid;
    const double current = mid_history_.back().mid;
    if (ref_mid == 0.0) return 0.0;
    return (current - ref_mid) / ref_mid * 100.0;
}

double FeatureExtractor::volatility_1min_log_returns_() const {
    if (mid_history_.size() < 2) return 0.0;
    const auto cutoff = mid_history_.back().t - std::chrono::seconds{60};

    // First sample inside the window (skip pre-roll).
    auto first = std::find_if(mid_history_.begin(), mid_history_.end(), [&](const auto& s) {
        return s.t >= cutoff;
    });
    if (first == mid_history_.end() || std::distance(first, mid_history_.end()) < 2) return 0.0;

    // Log-returns + Welford in one pass; we need stdev only.
    WelfordAccumulator<double> acc;
    auto prev = first;
    std::for_each(std::next(first), mid_history_.end(), [&](const auto& s) {
        if (prev->mid > 0.0 && s.mid > 0.0) {
            acc.update(std::log(s.mid / prev->mid));
        }
        prev = std::next(prev); // Manual advance for prev logic
    });
    return acc.stdev();
}

FeatureFrame FeatureExtractor::extract(std::chrono::system_clock::time_point now) {
    const auto t_total_start = std::chrono::high_resolution_clock::now();

    FeatureFrame f{};
    f.ticker    = ticker_;
    f.timestamp = now;

    // ----- Order book stage (timed) -----
    const auto t_book_start = std::chrono::high_resolution_clock::now();
    if (ob_ != nullptr) {
        const auto bb = ob_->best_bid();
        const auto ba = ob_->best_ask();
        const auto m  = ob_->mid();
        if (bb) f.best_bid = *bb;
        if (ba) f.best_ask = *ba;
        if (m)  f.mid      = *m;
        if (bb && ba) {
            f.spread_abs = *ba - *bb;
            if (f.mid > 0.0) {
                f.spread_bps = f.spread_abs / f.mid * kBpsScale;
            }
        }
        f.bid_depth_10 = ob_->bid_depth(10);
        f.ask_depth_10 = ob_->ask_depth(10);
        const double sum_d = f.bid_depth_10 + f.ask_depth_10;
        f.imbalance = sum_d > 0.0
                          ? (f.bid_depth_10 - f.ask_depth_10) / sum_d
                          : 0.0;
    }
    const auto t_book_end = std::chrono::high_resolution_clock::now();
    hist_book_.record(std::chrono::duration_cast<std::chrono::microseconds>(
                          t_book_end - t_book_start).count());

    // ----- Trade stream stage -----
    if (ts_ != nullptr) {
        const auto stats = ts_->get_stats();
        f.buy_vol_1s    = stats.buy_vol_1s;
        f.buy_vol_5s    = stats.buy_vol_5s;
        f.buy_vol_30s   = stats.buy_vol_30s;
        f.sell_vol_1s   = stats.sell_vol_1s;
        f.sell_vol_5s   = stats.sell_vol_5s;
        f.sell_vol_30s  = stats.sell_vol_30s;
        f.prints_per_sec = stats.prints_per_sec;
        f.avg_print_size = stats.avg_size;
        const double tot5 = stats.buy_vol_5s + stats.sell_vol_5s;
        f.tape_aggression = tot5 > 0.0
                                ? (stats.buy_vol_5s - stats.sell_vol_5s) / tot5
                                : 0.0;
        f.max_print_size_5s = stats.q99_size;  // T-Digest q99 as upper-tail proxy
    }

    // ----- Maintain mid history before computing dynamics -----
    if (f.mid > 0.0) {
        if (mid_history_.size() >= cfg_.reserve_history) {
            mid_history_.pop_front();
        }
        mid_history_.push_back({now, f.mid});
    }

    // ----- Price dynamics -----
    f.price_change_1s   = price_change_pct_(now, std::chrono::seconds{1});
    f.price_change_5s   = price_change_pct_(now, std::chrono::seconds{5});
    f.price_change_30s  = price_change_pct_(now, std::chrono::seconds{30});
    f.volatility_1min   = volatility_1min_log_returns_();
    f.volatility_1min_bps = f.volatility_1min * kBpsScale;

    // ----- Leader -----
    if (lt_ != nullptr) {
        f.leader_correlation = lt_->correlation();
        f.leader_lag_ms      = lt_->lag_ms();
        // leader_change_*s are populated by the caller pipeline (we have only
        // the follower's own mid history here).
    }

    const auto t_total_end = std::chrono::high_resolution_clock::now();
    hist_total_.record(std::chrono::duration_cast<std::chrono::microseconds>(
                           t_total_end - t_total_start).count());
    return f;
}

}  // namespace trade_bot
