#pragma once

#include "strategy/TradePlan.hpp"
#include "features/FeatureFrame.hpp"

#include <string>
#include <vector>
#include <deque>
#include <mutex>

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

    void log_entry(const Entry& entry);
    std::vector<Entry> get_recent_entries(size_t count);
    std::size_t size() { 
        std::lock_guard lock(mtx_);
        return cache_.size(); 
    }

private:
    std::string get_current_date_str_();

    std::string log_dir_;
    std::mutex  mtx_;
    std::deque<Entry> cache_; // Keep last N in memory for dashboard
};

} // namespace trade_bot
