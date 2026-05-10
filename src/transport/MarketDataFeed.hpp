#pragma once

#include "domain/Types.hpp"
#include "IWsClient.hpp"
#include <functional>
#include <memory>
#include <vector>
#include <string>
#include <set>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <chrono>

namespace trade_bot {

struct FundingData {
    double rate{0.0};
    std::chrono::system_clock::time_point next_funding_time{};
    std::chrono::system_clock::time_point updated_at{};
};

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

    /// Returns the latest funding data for the ticker, or nullopt if not yet received.
    std::optional<FundingData> get_funding(const Ticker& ticker) const;

    /// Returns the latest mark price, or 0.0 if not yet received.
    double get_mark_price(const Ticker& ticker) const;

    /// Returns all cached mark prices with a single lock (avoids N separate
    /// mutex acquisitions when iterating universe tickers for dashboard).
    std::unordered_map<Ticker, double> get_all_mark_prices() const;

    // Optional tap invoked for every raw WS message before dispatch.
    // Callback receives (parsed_json, recv_ts_ns).  Used by DumpRecorder.
    using RawTap = std::function<void(const nlohmann::json&, int64_t)>;
    void set_record_tap(RawTap tap);

private:
    void handle_message(const nlohmann::json& j);
    void resubscribe_all();
    
    using ListenerList = std::vector<IMarketDataListener*>;
    std::shared_ptr<const ListenerList> get_target_listeners(const Ticker& ticker);
    void invalidate_cache_();

    std::shared_ptr<IWsClient> m_ws_client;
    int m_connection_id;

    std::set<Ticker> m_subscribed_tickers;
    std::vector<IMarketDataListener*> m_listeners;
    std::unordered_map<Ticker, std::vector<IMarketDataListener*>> m_ticker_listeners;

    // Cache for merged listeners (m_listeners + m_ticker_listeners[ticker])
    // Shared pointer ensures thread-safe dispatch if cache is invalidated during iteration.
    std::unordered_map<Ticker, std::shared_ptr<const ListenerList>> m_merged_cache;

    mutable std::mutex m_mutex;
    bool m_active = false;
    RawTap m_record_tap;

    // Funding and mark price caches (updated from WS)
    std::unordered_map<Ticker, FundingData> m_funding_cache;
    std::unordered_map<Ticker, double>      m_mark_price_cache;
};

} // namespace trade_bot
