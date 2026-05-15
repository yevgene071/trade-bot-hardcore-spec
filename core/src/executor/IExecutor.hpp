#pragma once

#include "strategy/TradePlan.hpp"
#include "trading/ActiveTrade.hpp"
#include "utils/FixedString.hpp"
#include "transport/MarketDataFeed.hpp"
#include <vector>

namespace trade_bot {

/**
 * Interface for executing trade plans.
 */
class IExecutor : public IMarketDataListener {
public:
    virtual ~IExecutor() = default;

    struct ClosedTrade {
        TradePlan   plan;
        double      entry_price{0.0};
        double      exit_price{0.0};
        double      size_filled{0.0};
        double      pnl_usd{0.0};
        FixedString<32> reason;
    };

    /// Submit a new trade plan for execution.
    virtual void submit(const TradePlan& plan) = 0;

    /// Cancel all active orders for a ticker.
    virtual void cancel_all(const Ticker& ticker) = 0;

    /// Inject recovered trades from state persistence or server-side orphans.
    virtual void inject_recovered_trades(const std::vector<ActiveTrade>& trades) = 0;
    
    /// Get all currently active trades (for lifecycle/monitoring).
    virtual std::vector<ActiveTrade> get_active_trades() const = 0;

    /// Drain the closed-trade buffer.
    virtual std::vector<ClosedTrade> pop_closed_trades() = 0;

    /// Update internal state based on latest market data or timers.
    virtual void tick(std::chrono::system_clock::time_point now) = 0;

    /// Inject exchange mark prices for PnL calculation.
    virtual void set_mark_prices(const std::unordered_map<Ticker, double>& marks) = 0;

    /// Close an active trade for the given ticker (strategy-requested invalidation).
    virtual void close_trade(const Ticker& ticker, const FixedString<32>& reason) = 0;
};

} // namespace trade_bot
