#pragma once

#include "IExecutor.hpp"
#include "marketdata/OrderBook.hpp"
#include "trading/ActiveTrade.hpp"
#include "transport/MarketDataFeed.hpp"

#include <map>
#include <unordered_map>
#include <vector>
#include <mutex>

namespace trade_bot {

/**
 * T3-PAPER: Emulated executor for paper trading.
 */
class PaperExecutor : public IExecutor {
public:
    struct Config {
        double slippage_bps{1.0};
    };

    explicit PaperExecutor(const std::map<Ticker, const OrderBook*>& books, Config cfg);
    explicit PaperExecutor(const std::map<Ticker, const OrderBook*>& books);

    void submit(const TradePlan& plan) override;
    void cancel_all(const Ticker& ticker) override;
    void inject_recovered_trades(const std::vector<ActiveTrade>& trades) override;
    void close_trade(const Ticker& ticker, const FixedString<32>& reason) override;

    // IMarketDataListener
    void on_trade(const Ticker& ticker, const Trade& trade) override;
    void on_orderbook_snapshot(const OrderBookSnapshot& snap) override;
    void on_orderbook_update(const OrderBookUpdate& upd) override;
    void on_order_update(const OrderUpdate&) override {}
    void on_position_update(const PositionUpdate&) override {}
    void on_balance_update(const BalanceUpdate&) override {}
    void on_finres_update(const FinresUpdate&) override {}
    void on_error(const std::string&) override {}

    /// Update internal state based on latest market data.
    void tick(std::chrono::system_clock::time_point now) override;
    void tick(const Ticker& ticker, std::chrono::system_clock::time_point now);

    std::vector<ActiveTrade> get_active_trades() const override;

    struct Position {
        TradePlan plan;
        double    executed_size;
        double    avg_price;
        std::chrono::system_clock::time_point opened_at;
    };

    /// Drain the closed-trade buffer (called every tick from BotApp).
    std::vector<ClosedTrade> pop_closed_trades() override;

    /// Sum of unrealized PnL across all open positions using mid price from books.
    double unrealized_pnl() const;

    /// Inject exchange mark prices so unrealized PnL uses the same price the
    /// dashboard displays (avoids mid vs mark discrepancy).
    void set_mark_prices(const std::unordered_map<Ticker, double>& marks) override { mark_prices_ = marks; }

    const std::map<Ticker, Position>& positions() const { return positions_; }

private:
    const std::map<Ticker, const OrderBook*>& books_;
    Config cfg_;
    mutable std::mutex mutex_;  // guards closed_trades_ (simultaneous close_trade + pop_closed_trades)
    std::vector<TradePlan>  pending_entries_;
    std::map<Ticker, Position> positions_;
    std::vector<ClosedTrade>   closed_trades_;
    std::unordered_map<Ticker, double> mark_prices_;
};

} // namespace trade_bot
