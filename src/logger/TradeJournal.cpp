#include "TradeJournal.hpp"
#include <nlohmann/json.hpp>
#include <fstream>
#include <filesystem>
#include <iomanip>

namespace trade_bot {

TradeJournal::TradeJournal(std::string log_dir) 
    : log_dir_(std::move(log_dir)) {
    std::filesystem::create_directories(log_dir_);
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
    
    // Get filename by date YYYY-MM-DD
    auto now = std::chrono::system_clock::now();
    auto in_time_t = std::chrono::system_clock::to_time_t(now);
    std::stringstream ss;
    ss << std::put_time(std::localtime(&in_time_t), "%Y-%m-%d");
    std::string filename = log_dir_ + "/" + ss.str() + ".jsonl";
    
    std::lock_guard<std::mutex> lk(mtx_);
    std::ofstream out(filename, std::ios::app);
    out << j.dump() << std::endl;

    cache_.push_back(entry);
    if (cache_.size() > 100) {
        cache_.erase(cache_.begin());
    }
}

std::vector<TradeJournal::Entry> TradeJournal::get_recent_entries(size_t count) {
    std::lock_guard<std::mutex> lk(mtx_);
    std::vector<Entry> res;
    size_t start = (cache_.size() > count) ? cache_.size() - count : 0;
    for (size_t i = start; i < cache_.size(); ++i) {
        res.push_back(cache_[i]);
    }
    std::reverse(res.begin(), res.end());
    return res;
}

} // namespace trade_bot
