#include "ClusterSnapshot.hpp"
#include "logger/Logger.hpp"
#include "transport/external/ExternalIoContext.hpp"

#include <random>

namespace trade_bot {

ClusterSnapshotManager::ClusterSnapshotManager(ClusterSnapshotClient& client,
                                             Config cfg)
    : client_(client)
    , cfg_(std::move(cfg)) {}

ClusterSnapshotManager::ClusterSnapshotManager(ClusterSnapshotClient& client)
    : ClusterSnapshotManager(client, Config{}) {}

ClusterSnapshotManager::~ClusterSnapshotManager() {
    stop();
}

void ClusterSnapshotManager::start() {
    bool expected = false;
    if (running_.compare_exchange_strong(expected, true)) {
        timer_ = std::make_unique<boost::asio::steady_timer>(ExternalIoContext::instance().context());
        schedule_poll_();
    }
}

void ClusterSnapshotManager::stop() {
    bool expected = true;
    if (running_.compare_exchange_strong(expected, false)) {
        if (timer_) {
            timer_->cancel();
        }
    }
}

void ClusterSnapshotManager::set_on_snapshot(OnSnapshot cb) {
    on_snapshot_ = std::move(cb);
}

void ClusterSnapshotManager::refresh(const std::vector<Ticker>& active_tickers) {
    std::lock_guard<std::mutex> lk(active_tickers_mtx_);
    active_tickers_ = active_tickers;
}

std::optional<ClusterSnapshot> ClusterSnapshotManager::get(const Ticker& ticker,
                                                         const std::string& timeframe) const {
    std::lock_guard<std::mutex> lk(cache_mtx_);
    auto it = cache_.find(ticker);
    if (it == cache_.end()) return std::nullopt;
    auto tit = it->second.find(timeframe);
    if (tit == it->second.end()) return std::nullopt;
    return tit->second;
}

void ClusterSnapshotManager::schedule_poll_() {
    if (!running_) return;

    std::mt19937_64 rng{std::random_device{}()};
    std::uniform_real_distribution<double> jitter_dist(
        1.0 - cfg_.poll_jitter_pct, 1.0 + cfg_.poll_jitter_pct);
    
    double sleep_sec = cfg_.poll_interval_sec.count() * jitter_dist(rng);
    timer_->expires_after(std::chrono::milliseconds(static_cast<int64_t>(sleep_sec * 1000)));
    
    timer_->async_wait([this](const boost::system::error_code& ec) {
        if (!ec) {
            poll_();
            schedule_poll_();
        }
    });
}

void ClusterSnapshotManager::poll_() {
    std::vector<Ticker> tickers;
    {
        std::lock_guard<std::mutex> lk(active_tickers_mtx_);
        tickers = active_tickers_;
    }

    for (const auto& ticker : tickers) {
        if (!running_) break;
        refresh_ticker_(ticker);
    }
}

void ClusterSnapshotManager::refresh_ticker_(const Ticker& ticker) {
    for (const auto& tf : cfg_.poll_timeframes) {
        if (!running_) break;
        
        // We use the ExternalIoContext to perform the actual fetch in the background 
        // to avoid blocking the timer's thread (though here it's the same ExternalIoContext).
        // Since ClusterSnapshotClient::fetch is synchronous, we are still blocking 
        // one of the threads. A better fix would be making ClusterSnapshotClient async.
        // For now, this is already better as it's interruptible and managed by asio.
        
        auto snap = client_.fetch(ticker, tf);
        if (snap) {
            {
                std::lock_guard<std::mutex> lk(cache_mtx_);
                cache_[ticker][tf] = *snap;
            }
            if (on_snapshot_) {
                on_snapshot_(*snap);
            }
            LOG_DEBUG("ClusterSnapshotManager: updated {} {}", ticker, tf);
        }
    }
}

} // namespace trade_bot
