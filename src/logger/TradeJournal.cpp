#include "TradeJournal.hpp"
#include "logger/Logger.hpp"
#include <nlohmann/json.hpp>
#include <fstream>
#include <filesystem>
#include <iomanip>
#include <ctime>
#include <algorithm>

namespace trade_bot {

TradeJournal::TradeJournal(std::string log_dir) 
    : log_dir_(std::move(log_dir)) {
    std::filesystem::create_directories(log_dir_);
}

std::string TradeJournal::get_current_date_str_() {
    auto now = std::chrono::system_clock::now();
    auto in_time_t = std::chrono::system_clock::to_time_t(now);
    std::tm buf;
#ifdef _WIN32
    gmtime_s(&buf, &in_time_t);
#else
    gmtime_r(&in_time_t, &buf);
#endif
    std::stringstream ss;
    ss << std::put_time(&buf, "%Y-%m-%d");
    return ss.str();
}

void TradeJournal::log_entry(const Entry& entry) {
    nlohmann::json j;
    
    // Simplified serialization
    j["ticker"] = entry.plan.ticker;
    j["strategy"] = entry.plan.strategy_name;
    j["side"] = (entry.plan.side == Side::Buy) ? "Buy" : "Sell";
    j["entry_price"] = entry.plan.entry_price;
    j["pnl"] = entry.pnl_usd;
    j["duration"] = entry.duration_sec;
    j["reason"] = entry.plan.reason;
    j["cause_of_exit"] = entry.cause_of_exit;
    
    std::string filename = log_dir_ + "/" + get_current_date_str_() + ".jsonl";
    
    std::lock_guard<std::mutex> lk(mtx_);
    std::ofstream out(filename, std::ios::app);
    out << j.dump() << "\n";
    out.flush();
    if (!out.good()) {
        LOG_ERROR("TradeJournal: write failed to {}", filename);
        return;
    }

    cache_.push_back(entry);
    if (cache_.size() > 100) {
        cache_.pop_front();
    }
}

std::vector<TradeJournal::Entry> TradeJournal::get_recent_entries(size_t count) {
    std::lock_guard<std::mutex> lk(mtx_);
    std::vector<Entry> res;
    size_t actual_count = std::min(count, cache_.size());
    
    // We want the most recent 'count' entries, which are at the end of the deque
    auto it = cache_.rbegin();
    for (size_t i = 0; i < actual_count; ++i, ++it) {
        res.push_back(*it);
    }
    // Resulting res is already in reverse order (most recent first)
    return res;
}

} // namespace trade_bot
