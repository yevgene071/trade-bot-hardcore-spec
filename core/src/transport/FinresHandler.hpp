#pragma once

#include "domain/Types.hpp"
#include "transport/MarketDataFeed.hpp"
#include "transport/IClock.hpp" // Добавляем заголовок часов
#include <map>
#include <mutex>
#include <memory>

namespace trade_bot {

class FinresHandler : public IMarketDataListener {
public:
    // Передаем абстракцию часов (опционально, для работы в реплее)
    explicit FinresHandler(std::shared_ptr<IClock> clock = nullptr);

    struct State {
        double current_result{0.0};
        double day_start_result{0.0};
        int64_t last_day_start_ts{0};
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
    std::shared_ptr<IClock> clock_; // Хранилище часов
};

} // namespace trade_bot
