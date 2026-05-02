#include "BinanceFundingClient.hpp"
#include "logger/Logger.hpp"
#include <nlohmann/json.hpp>

namespace trade_bot {

BinanceFundingClient::BinanceFundingClient(std::shared_ptr<IHttpClient> http)
    : http_(std::move(http)) {}

bool BinanceFundingClient::is_stale(std::chrono::seconds max_age) const {
    std::shared_lock lock(mutex_);
    if (funding_map_.empty()) return true;
    
    auto now = std::chrono::system_clock::now();
    // Check if any active ticker is stale
    for (const auto& [ticker, feed_val] : funding_map_) {
        if (now - feed_val.timestamp > max_age) return true;
    }
    return false;
}

std::optional<FundingData> BinanceFundingClient::get_funding(const std::string& ticker) const {
    std::shared_lock lock(mutex_);
    auto it = funding_map_.find(ticker);
    if (it != funding_map_.end()) {
        return it->second.value;
    }
    return std::nullopt;
}

void BinanceFundingClient::update_funding(const std::string& ticker, FundingData data) {
    std::unique_lock lock(mutex_);
    funding_map_[ticker] = {data, std::chrono::system_clock::now()};
}

void BinanceFundingClient::start_polling(boost::asio::io_context& ioc, std::chrono::seconds interval) {
    poll_interval_ = interval;
    timer_ = std::make_unique<boost::asio::steady_timer>(ioc);
    schedule_poll(std::chrono::seconds(0)); // Start immediately
}

void BinanceFundingClient::schedule_poll(std::chrono::seconds interval) {
    timer_->expires_after(interval);
    timer_->async_wait([this](const boost::system::error_code& ec) {
        if (!ec) {
            poll();
            schedule_poll(poll_interval_);
        }
    });
}

void BinanceFundingClient::poll() {
    try {
        auto resp = http_->get("https://fapi.binance.com/fapi/v1/premiumIndex");
        if (resp.status != 200) {
            LOG_WARN("BinanceFundingClient: failed to fetch funding, status: {}", resp.status);
            return;
        }

        auto j = nlohmann::json::parse(resp.body);
        if (j.is_array()) {
            for (const auto& item : j) {
                std::string symbol = item.value("symbol", "");
                if (symbol.empty()) continue;

                try {
                    double rate = std::stod(item.value("lastFundingRate", "0.0"));
                    int64_t next_ts_ms = item.value("nextFundingTime", 0L);
                    
                    FundingData data;
                    data.rate = rate;
                    data.next_funding_time = std::chrono::system_clock::time_point(std::chrono::milliseconds(next_ts_ms));
                    
                    update_funding(symbol, data);
                } catch (const std::exception& e) {
                    LOG_WARN("BinanceFundingClient: error parsing item for {}: {}", symbol, e.what());
                }
            }
        }
    } catch (const std::exception& e) {
        LOG_ERROR("BinanceFundingClient: poll error: {}", e.what());
    }
}

} // namespace trade_bot
