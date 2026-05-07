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

    PaperExecutor(const std::map<Ticker, const OrderBook*>& books, Config cfg);
    PaperExecutor(const std::map<Ticker, const OrderBook*>& books);

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

    const std::map<Ticker, Position>& positions() const { return positions_; }

private:
    const std::map<Ticker, const OrderBook*>& books_;
    Config cfg_;

    std::vector<TradePlan> pending_entries_;
    std::map<Ticker, Position> positions_;
};

} // namespace trade_bot
