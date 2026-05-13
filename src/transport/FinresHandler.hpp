#pragma once

#include "domain/Types.hpp"
#include "transport/MarketDataFeed.hpp"
#include <map>
#include <mutex>

namespace trade_bot {

class FinresHandler : public IMarketDataListener {
public:
    struct State {
        double current_result{0.0};
        double day_start_result{0.0};
        bool   initialized{false};
    };

    struct Snapshot {
        double balance;
        double equity; // For simplicity in this project, we can treat result as balance if equity info is not separate
    };

    void on_trade(const Ticker&, const Trade&) override {}
    void on_orderbook_snapshot(const OrderBookSnapshot&) override {}
    void on_orderbook_update(const OrderBookUpdate&) override {}
    void on_order_update(const OrderUpdate&) override {}
    void on_position_update(const PositionUpdate&) override {}
    void on_balance_update(const BalanceUpdate&) override {}
    void on_error(const std::string&) override {}

    void on_finres_update(const FinresUpdate& update) override {
        handle_update(update);
    }

    void handle_update(const FinresUpdate& update);
    void reset_day_start();

    double get_realized_pnl(int connection_id) const;
    bool   is_ready(int connection_id) const;
    
    std::optional<Snapshot> get_snapshot(int connection_id) const;

private:
    mutable std::mutex mutex_;
    std::map<int, State> states_;
};

} // namespace trade_bot
