#include "TradeJournal.hpp"
#include "logger/Logger.hpp"
#include "numeric/Kahan.hpp"
#include <nlohmann/json.hpp>
#include <fcntl.h>
#include <unistd.h>
#include <filesystem>
#include <iomanip>
#include <ctime>
#include <algorithm>

namespace trade_bot {

TradeJournal::TradeJournal(std::string log_dir) 
    : log_dir_(std::move(log_dir)) {
    std::filesystem::create_directories(log_dir_);
    worker_ = std::thread(&TradeJournal::worker_thread_, this);
}

TradeJournal::~TradeJournal() {
    {
        std::lock_guard<std::mutex> lk(mtx_);
        stop_ = true;
    }
    cv_.notify_all();
    if (worker_.joinable()) {
        worker_.join();
    }
    if (current_fd_ >= 0) {
        ::close(current_fd_);
    }
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
    std::lock_guard<std::mutex> lk(mtx_);
    cache_.push_back(entry);
    if (cache_.size() > 200) cache_.pop_front();
    
    queue_.push_back(entry);
    cv_.notify_one();
}

void TradeJournal::worker_thread_() {
    while (true) {
        std::vector<Entry> batch;
        {
            std::unique_lock<std::mutex> lk(mtx_);
            cv_.wait(lk, [this] { return stop_ || !queue_.empty(); });
            if (stop_ && queue_.empty()) break;
            
            while (!queue_.empty()) {
                batch.push_back(std::move(queue_.front()));
                queue_.pop_front();
            }
        }
        
        for (const auto& entry : batch) {
            nlohmann::json j;
            j["ts_unix_ms"]    = entry.ts_unix_ms;
            j["ticker"]        = entry.plan.ticker;
            j["strategy"]      = entry.plan.strategy_name;
            j["side"]          = (entry.plan.side == Side::Buy) ? "Buy" : "Sell";
            j["entry_type"]    = (entry.plan.entry_type == OrderType::Market) ? "Market" : "Limit";
            j["entry_price"]   = entry.plan.entry_price;
            j["exit_price"]    = entry.exit_price;
            j["stop_price"]    = entry.plan.stop_price;
            j["tp1_price"]     = entry.plan.tp1_price;
            j["size_coin"]     = entry.plan.size_coin;
            j["risk_usd"]      = entry.plan.risk_usd;
            j["pnl_usd"]       = entry.pnl_usd;
            j["duration_sec"]  = entry.duration_sec;
            j["reason"]        = entry.plan.reason;
            j["cause_of_exit"] = entry.cause_of_exit;

            std::string line = j.dump() + "\n";
            
            std::string date = get_current_date_str_();
            if (date != current_date_ || current_fd_ < 0) {
                if (current_fd_ >= 0) ::close(current_fd_);
                current_date_ = date;
                std::string filename = log_dir_ + "/" + current_date_ + ".jsonl";
                current_fd_ = ::open(filename.c_str(), O_WRONLY | O_APPEND | O_CREAT, 0644);
            }

            if (current_fd_ >= 0) {
                if (::write(current_fd_, line.data(), line.size()) < 0) {
                    // fall back to stderr or log if possible
                } else {
                    ::fdatasync(current_fd_);
                }
            }
        }   // for
    }   // while
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

std::vector<TradeJournal::Entry> TradeJournal::get_all_entries() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return std::vector<Entry>(cache_.begin(), cache_.end());
}

} // namespace trade_bot
