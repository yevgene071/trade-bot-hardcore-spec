#pragma once

#include "domain/Types.hpp"
#include "IWsClient.hpp"
#include <functional>
#include <memory>
#include <vector>
#include <string>
#include <mutex>
#include <atomic>
#include <optional>
#include "absl/container/btree_map.h"
#include "absl/container/btree_set.h"
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
    virtual void force_resync_orderbook(const Ticker& ticker);

    using OrderBookSnapshotFetcher =
        std::function<OrderBookSnapshot(const Ticker& ticker,
                                        int zoom_index,
                                        std::optional<int> depth_levels,
                                        std::optional<double> depth_percent)>;
    void set_orderbook_snapshot_fetcher(OrderBookSnapshotFetcher fetcher);

    virtual void start();
    virtual void stop();

    virtual bool is_connected() const { return m_ws_client && m_ws_client->is_connected(); }

    /// Returns the latest funding data for the ticker, or nullopt if not yet received.
    std::optional<FundingData> get_funding(const Ticker& ticker) const;

    /// Returns the latest mark price, or 0.0 if not yet received.
    double get_mark_price(const Ticker& ticker) const;

    /// Returns all cached mark prices with a single lock (avoids N separate
    /// mutex acquisitions when iterating universe tickers for dashboard).
    absl::btree_map<Ticker, double> get_all_mark_prices() const;

    // Optional tap invoked for every raw WS message before dispatch.
    // Callback receives (parsed_json, recv_ts_ns).  Used by DumpRecorder.
    using RawTap = std::function<void(const nlohmann::json&, int64_t)>;
    void set_record_tap(RawTap tap);

    // When true, skip orderbook_update and trade_update in handle_message()
    // because PipelineProcessor handles them via the raw path.
    void set_skip_hot_path(bool skip) { m_skip_hot_path_ = skip; }

private:
    void handle_message(const nlohmann::json& j);
    void resubscribe_all();
    bool try_rest_orderbook_snapshot_(const Ticker& ticker,
                                      int zoom_index,
                                      std::optional<int> depth_levels,
                                      std::optional<double> depth_percent);
    void send_orderbook_subscribe_(const Ticker& ticker, bool fetch_snapshot_false);
    void dispatch_orderbook_snapshot_(const OrderBookSnapshot& snapshot);
    
    using ListenerList = std::vector<IMarketDataListener*>;
    std::shared_ptr<const ListenerList> get_target_listeners(const Ticker& ticker);
    // Rebuilds the CoW listener snapshots from m_listeners/m_ticker_listeners.
    // Caller MUST hold m_mutex (writer side only).
    void rebuild_listener_snapshot_();

    std::shared_ptr<IWsClient> m_ws_client;
    int m_connection_id;

    // P0-DETERMINISM: btree_set for stable iteration order in resubscribe_all()
    absl::btree_set<Ticker> m_subscribed_tickers;
    absl::btree_set<Ticker> m_ws_snapshot_only;
    std::vector<IMarketDataListener*> m_listeners;
    absl::btree_map<Ticker, std::vector<IMarketDataListener*>> m_ticker_listeners;
    OrderBookSnapshotFetcher m_snapshot_fetcher;

    // CoW (copy-on-write) listener snapshots. Writers (add/remove_listener)
    // rebuild the whole map under m_mutex and publish via atomic store-release.
    // The hot dispatch path (get_target_listeners) does a single atomic
    // load-acquire — no mutex, no allocation.
    using MergedMap = absl::btree_map<Ticker, std::shared_ptr<const ListenerList>>;
    std::atomic<std::shared_ptr<const MergedMap>>    m_merged_snapshot;
    std::atomic<std::shared_ptr<const ListenerList>> m_global_listeners_snapshot;

    mutable std::mutex m_mutex;
    std::atomic<bool> m_active{false};
    bool m_skip_hot_path_{false};
    RawTap m_record_tap;

    // Funding and mark price caches (updated from WS). CoW atomic snapshots:
    // single-writer (processor/WS thread), multi-reader (dashboard) — lock-free reads.
    using FundingMap   = absl::btree_map<Ticker, FundingData>;
    using MarkPriceMap = absl::btree_map<Ticker, double>;
    std::atomic<std::shared_ptr<const FundingMap>>   m_funding_snapshot;
    std::atomic<std::shared_ptr<const MarkPriceMap>> m_mark_price_snapshot;
};

} // namespace trade_bot
