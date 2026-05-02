#pragma once

#include "domain/Types.hpp"
#include "IWsClient.hpp"
#include <memory>
#include <vector>
#include <string>
#include <set>
#include <mutex>
#include <unordered_map>

namespace trade_bot {

class IMarketDataListener {
public:
    virtual ~IMarketDataListener() = default;
    virtual void on_trade(const Ticker& ticker, const Trade& trade) = 0;
    virtual void on_trades(const Ticker& ticker, const std::vector<Trade>& trades) {
        for (const auto& t : trades) on_trade(ticker, t);
    }
    virtual void on_orderbook_snapshot(const OrderBookSnapshot& snapshot) = 0;
    virtual void on_orderbook_update(const OrderBookUpdate& update) = 0;
    virtual void on_order_update(const OrderUpdate& update) = 0;
    virtual void on_position_update(const PositionUpdate& update) = 0;
    virtual void on_balance_update(const BalanceUpdate& update) = 0;
    virtual void on_finres_update(const FinresUpdate& update) = 0;
    virtual void on_error(const std::string& msg) = 0;
};

class MarketDataFeed {
public:
    MarketDataFeed(std::shared_ptr<IWsClient> ws_client, int connection_id);
    virtual ~MarketDataFeed() = default;
    
    virtual void add_listener(IMarketDataListener* listener);
    virtual void add_listener(const Ticker& ticker, IMarketDataListener* listener);
    virtual void remove_listener(IMarketDataListener* listener);
    virtual void remove_listener(const Ticker& ticker, IMarketDataListener* listener);

    virtual void subscribe_ticker(const Ticker& ticker);
    virtual void unsubscribe_ticker(const Ticker& ticker);
    
    virtual void start();
    virtual void stop();

private:
    void handle_message(const nlohmann::json& j);
    void resubscribe_all();
    std::vector<IMarketDataListener*> get_target_listeners(const Ticker& ticker);
    
    std::shared_ptr<IWsClient> m_ws_client;
    int m_connection_id;
    
    std::set<Ticker> m_subscribed_tickers;
    std::vector<IMarketDataListener*> m_listeners;
    std::unordered_map<Ticker, std::vector<IMarketDataListener*>> m_ticker_listeners;
    std::mutex m_mutex;
    
    bool m_active = false;
};

} // namespace trade_bot
