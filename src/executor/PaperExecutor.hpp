#pragma once

#include "IExecutor.hpp"
#include "marketdata/OrderBook.hpp"
#include "trading/ActiveTrade.hpp"

#include <map>
#include <vector>

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

    /// Update internal state based on latest market data.
    void tick(std::chrono::system_clock::time_point now);

    std::vector<ActiveTrade> get_active_trades() const;

    struct Position {
        TradePlan plan;
        double    executed_size;
        double    avg_price;
    };

    struct ClosedTrade {
        TradePlan   plan;
        double      entry_price{0.0};
        double      exit_price{0.0};
        double      size_filled{0.0};
        double      pnl_usd{0.0};
        std::string reason;
    };

    /// Drain the closed-trade buffer (called every tick from BotApp).
    std::vector<ClosedTrade> pop_closed_trades();

    /// Sum of unrealized PnL across all open positions using mid price from books.
    double unrealized_pnl() const;

    const std::map<Ticker, Position>& positions() const { return positions_; }

private:
    const std::map<Ticker, const OrderBook*>& books_;
    Config cfg_;

    std::vector<TradePlan>  pending_entries_;
    std::map<Ticker, Position> positions_;
    std::vector<ClosedTrade>   closed_trades_;
};

} // namespace trade_bot
