#pragma once

#include "strategy/TradePlan.hpp"
#include "features/FeatureFrame.hpp"

#include <string>
#include <vector>
#include <deque>
#include <mutex>
#include <thread>
#include <condition_variable>
#include <atomic>

namespace trade_bot {

/**
 * T3-JOURNAL: Logs completed or proposed trades to JSONL for analysis.
 */
class TradeJournal {
public:
    struct Entry {
        TradePlan plan;
        FeatureFrame frame_at_entry;
        double pnl_usd{0.0};
        double duration_sec{0.0};
        double exit_price{0.0};
        std::string cause_of_exit;
        int64_t ts_unix_ms{0}; // timestamp of entry creation for dashboard display
    };

    explicit TradeJournal(std::string log_dir = "journal");
    ~TradeJournal();

    void log_entry(const Entry& entry);
    std::vector<Entry> get_recent_entries(size_t count);
    
    /// Returns all cached entries (for determinism testing).
    /// Entries are in chronological order (oldest first).
    std::vector<Entry> get_all_entries() const;
    std::size_t size() { 
        std::lock_guard lock(mtx_);
        return cache_.size(); 
    }

private:
    std::string get_current_date_str_();

    std::string log_dir_;
    mutable std::mutex  mtx_;
    std::deque<Entry> cache_; // Keep last N in memory for dashboard
    int         current_fd_{-1};
    std::string current_date_;

    std::deque<Entry>   queue_;
    std::condition_variable cv_;
    std::thread         worker_;
    std::atomic<bool>   stop_{false};
    void worker_thread_();
};

} // namespace trade_bot
