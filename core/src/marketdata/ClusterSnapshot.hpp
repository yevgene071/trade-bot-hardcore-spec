#pragma once

#include "domain/Types.hpp"
#include "transport/ClusterSnapshotClient.hpp"

#include <atomic>
#include <chrono>
#include <functional>
#include <map>
#include <mutex>
#include <random>
#include <string>
#include <vector>
#include <boost/asio/steady_timer.hpp>

namespace trade_bot {

/**
 * T1-CLUSTER: Poller and cache for footprint data.
 */
class ClusterSnapshotManager {
public:
    struct Config {
        std::chrono::seconds poll_interval_sec{60};
        std::vector<std::string> poll_timeframes{"M5", "M15", "H1"};
        double poll_jitter_pct{0.1}; // 10% jitter
        int max_concurrent_requests{2};
    };

    using OnSnapshot = std::function<void(const ClusterSnapshot&)>;

    ClusterSnapshotManager(ClusterSnapshotClient& client, Config cfg);
    explicit ClusterSnapshotManager(ClusterSnapshotClient& client);
    virtual ~ClusterSnapshotManager();

    void start();
    void stop();

    void set_on_snapshot(OnSnapshot cb);

    /// Manually trigger a refresh for all active tickers.
    void refresh(const std::vector<Ticker>& active_tickers);

    /// Get cached snapshot.
    virtual std::optional<ClusterSnapshot> get(const Ticker& ticker,
                                              const std::string& timeframe) const;

private:
    void schedule_poll_();
    void poll_();
    void refresh_ticker_(const Ticker& ticker);

    ClusterSnapshotClient& client_;
    Config                 cfg_;
    OnSnapshot             on_snapshot_;
    
    std::atomic<bool>      running_{false};
    std::unique_ptr<boost::asio::steady_timer> timer_;
    
    mutable std::mutex     cache_mtx_;
    // ticker -> timeframe -> snapshot
    std::map<Ticker, std::map<std::string, ClusterSnapshot>> cache_;
    
    std::vector<Ticker>    active_tickers_;
    mutable std::mutex     active_tickers_mtx_;

    // V1: reuse rng across schedule_poll_ calls — random_device is expensive
    std::mt19937_64        rng_{std::random_device{}()};
};

} // namespace trade_bot
