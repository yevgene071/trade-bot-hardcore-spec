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
    , hist_total_(cfg.latency_max_us, cfg.hdr_significant_digits) {
    mid_history_.resize(cfg_.reserve_history);
}

void FeatureExtractor::set_sources(const OrderBook* ob,
                                   const TradeStream* ts,
                                   const LeaderTracker* lt) {
    ob_ = ob;
    ts_ = ts;
    lt_ = lt;
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
        mid_history_[mid_head_] = {now, f.mid};
        mid_head_ = (mid_head_ + 1) % cfg_.reserve_history;
        if (mid_count_ < cfg_.reserve_history) mid_count_++;
    }

    // ----- Price dynamics & Volatility (Single Pass) -----
    if (mid_count_ >= 2) {
        const auto t1 = now - std::chrono::seconds{1};
        const auto t5 = now - std::chrono::seconds{5};
        const auto t30 = now - std::chrono::seconds{30};
        const auto t60 = now - std::chrono::seconds{60};

        double mid1 = 0, mid5 = 0, mid30 = 0;
        WelfordAccumulator<double> vol_acc;
        
        // Scan backwards in circular buffer
        double current_mid = f.mid;
        double last_mid = current_mid;
        
        for (std::size_t i = 0; i < mid_count_; ++i) {
            std::size_t idx = (mid_head_ + cfg_.reserve_history - 1 - i) % cfg_.reserve_history;
            const auto& sample = mid_history_[idx];
            
            if (i > 0) { // Skip the very first (current) sample which we already have in last_mid
                if (sample.t >= t1) mid1 = sample.mid;
                if (sample.t >= t5) mid5 = sample.mid;
                if (sample.t >= t30) mid30 = sample.mid;
                if (sample.t >= t60) {
                    if (sample.mid > 0 && last_mid > 0) {
                        vol_acc.update(std::log(last_mid / sample.mid));
                    }
                } else {
                    break;
                }
                last_mid = sample.mid;
            }
        }

        auto calc_pct = [current_mid](double ref) {
            return (ref > 0) ? (current_mid - ref) / ref * 100.0 : 0.0;
        };

        f.price_change_1s = calc_pct(mid1 > 0 ? mid1 : mid_history_[(mid_head_ + cfg_.reserve_history - mid_count_) % cfg_.reserve_history].mid);
        f.price_change_5s = calc_pct(mid5 > 0 ? mid5 : mid_history_[(mid_head_ + cfg_.reserve_history - mid_count_) % cfg_.reserve_history].mid);
        f.price_change_30s = calc_pct(mid30 > 0 ? mid30 : mid_history_[(mid_head_ + cfg_.reserve_history - mid_count_) % cfg_.reserve_history].mid);
        f.volatility_1min = vol_acc.stdev();
    }
    
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
