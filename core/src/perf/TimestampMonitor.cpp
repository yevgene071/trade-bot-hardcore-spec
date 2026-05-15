#include "TimestampMonitor.hpp"

#include "logger/Logger.hpp"

#include <algorithm>

namespace trade_bot {

TimestampMonitor::TimestampMonitor() : TimestampMonitor(Config{}) {}

TimestampMonitor::TimestampMonitor(Config cfg)
    : cfg_(cfg)
    , latency_(cfg.highest_latency_us, cfg.sig_digits) {}

void TimestampMonitor::on_trade(const Ticker& ticker, const Trade& /*trade*/) {
    record_pair(ticker, clock::now(), /*from_trade=*/true);
}

void TimestampMonitor::on_trades(const Ticker& ticker, const std::vector<Trade>& trades) {
    if (trades.empty()) {
        return;
    }
    // Treat the burst as a single arrival — using the latest event's wall
    // time. That avoids multiplying samples for batched feeds.
    record_pair(ticker, clock::now(), /*from_trade=*/true);
}

void TimestampMonitor::on_orderbook_snapshot(const OrderBookSnapshot& snapshot) {
    record_pair(snapshot.ticker, clock::now(), /*from_trade=*/false);
}

void TimestampMonitor::on_orderbook_update(const OrderBookUpdate& update) {
    record_pair(update.ticker, clock::now(), /*from_trade=*/false);
}

void TimestampMonitor::record_pair(const Ticker& ticker, clock::time_point now, bool from_trade) {
    std::lock_guard<std::mutex> lk(mtx_);
    auto& st = per_ticker_[ticker];

    auto record_delta = [&](clock::time_point counterpart) {
        const auto delta_us =
            std::chrono::duration_cast<std::chrono::microseconds>(now - counterpart).count();
        if (delta_us < 0 || delta_us > cfg_.pair_window_us) {
            return;
        }
        const int64_t clamped = std::min<int64_t>(delta_us, cfg_.highest_latency_us);
        latency_.record(clamped);
        ++samples_since_check_;
        if (samples_since_check_ >= cfg_.warn_check_every_n) {
            samples_since_check_ = 0;
            check_thresholds_locked();
        }
    };

    if (from_trade) {
        if (st.has_book) {
            record_delta(st.last_book);
        }
        st.last_trade = now;
        st.has_trade = true;
    } else {
        if (st.has_trade) {
            record_delta(st.last_trade);
        }
        st.last_book = now;
        st.has_book = true;
    }
}

void TimestampMonitor::check_thresholds_locked() {
    const int64_t p99 = latency_.value_at_percentile(99.0);
    const double jitter = latency_.stddev();
    if (p99 > cfg_.warn_p99_us) {
        LOG_WARN("trade_orderbook_latency p99={} us exceeds threshold {} us",
                 p99, cfg_.warn_p99_us);
    }
    if (jitter > static_cast<double>(cfg_.warn_jitter_us)) {
        LOG_WARN("trade_orderbook_jitter={:.1f} us exceeds threshold {} us",
                 jitter, cfg_.warn_jitter_us);
    }
}

int64_t TimestampMonitor::latency_p99_us() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return latency_.value_at_percentile(99.0);
}

int64_t TimestampMonitor::latency_p50_us() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return latency_.value_at_percentile(50.0);
}

double TimestampMonitor::jitter_us() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return latency_.stddev();
}

int64_t TimestampMonitor::sample_count() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return latency_.total_count();
}

void TimestampMonitor::reset() {
    std::lock_guard<std::mutex> lk(mtx_);
    latency_.reset();
    per_ticker_.clear();
    samples_since_check_ = 0;
}

} // namespace trade_bot
