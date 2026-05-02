#pragma once

#include "IExecutor.hpp"
#include "transport/IOrderGateway.hpp"
#include "transport/MarketDataFeed.hpp"
#include "trading/ActiveTrade.hpp"
#include "trading/OrderReconciliator.hpp"

#include <map>
#include <mutex>
#include <vector>
#include <atomic>

namespace trade_bot {

/**
 * T4-EXECUTOR: Live order management.
 */
class LiveExecutor : public IExecutor, public IMarketDataListener {
public:
    struct Config {
        double min_partial_fill_ratio{0.5};
        int    exchange_error_streak_limit{5};
        std::chrono::milliseconds balance_reservation_timeout{1000};
    };

    LiveExecutor(int connection_id, IOrderGateway& gateway, MarketDataFeed& feed, Config cfg);
    LiveExecutor(int connection_id, IOrderGateway& gateway, MarketDataFeed& feed);

    // IExecutor
    void submit(const TradePlan& plan) override;
    void cancel_all(const Ticker& ticker) override;

    // IMarketDataListener
    void on_trade(const Ticker&, const Trade&) override {}
    void on_orderbook_snapshot(const OrderBookSnapshot&) override {}
    void on_orderbook_update(const OrderBookUpdate&) override {}
    void on_order_update(const OrderUpdate& update) override;
    void on_position_update(const PositionUpdate& update) override;
    void on_balance_update(const BalanceUpdate& update) override;
    void on_finres_update([[maybe_unused]] const FinresUpdate& update) override {}
    void on_error(const std::string& msg) override;

    void tick(std::chrono::system_clock::time_point now);

    void set_alert_callback(std::function<void(const std::string&)> cb) { alert_cb_ = cb; }

    const std::map<Ticker, std::vector<ActiveTrade>>& active_trades() const { return trades_; }

private:
    void handle_reconciled_(const ReconcileResult& res);
    void place_stops_(ActiveTrade& trade);
    void update_balance_reservation_(double amount, bool add);

    int connection_id_;
    IOrderGateway& gateway_;
    MarketDataFeed& feed_;
    Config cfg_;

    mutable std::mutex mutex_;
    std::map<Ticker, std::vector<ActiveTrade>> trades_;
    
    OrderReconciliator reconciliator_;
    std::atomic<int> error_streak_{0};
    
    double reserved_balance_usd_{0.0};
    std::chrono::system_clock::time_point last_balance_update_;
    std::function<void(const std::string&)> alert_cb_;
};

} // namespace trade_bot
