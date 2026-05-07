#pragma once

#include "transport/external/IExternalFeed.hpp"
#include "transport/IHttpClient.hpp"
#include <map>
#include <memory>
#include <shared_mutex>
#include <chrono>
#include <boost/asio.hpp>

namespace trade_bot {

struct FundingData {
    double rate;
    std::chrono::system_clock::time_point next_funding_time;
};

class BinanceFundingClient : public IExternalFeed {
public:
    explicit BinanceFundingClient(std::shared_ptr<IHttpClient> http);
    
    std::string name() const override { return "BinanceFunding"; }
    bool is_stale(std::chrono::seconds max_age) const override;
    
    std::optional<FundingData> get_funding(const std::string& ticker) const;
    void update_funding(const std::string& ticker, FundingData data);

    void add_ticker(const std::string& ticker);
    void remove_ticker(const std::string& ticker);

    void start_polling(boost::asio::io_context& ioc, std::chrono::seconds interval);

private:
    void poll();
    void schedule_poll(std::chrono::seconds interval);

    std::shared_ptr<IHttpClient> http_;
    mutable std::shared_mutex mutex_;
    std::map<std::string, FeedValue<FundingData>> funding_map_;
    std::set<std::string> active_tickers_;
    
    std::unique_ptr<boost::asio::steady_timer> timer_;
    std::chrono::seconds poll_interval_{60};
};

} // namespace trade_bot
