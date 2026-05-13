#include "PaperExecutor.hpp"
#include "logger/Logger.hpp"

#include <cmath>
#include <algorithm>

namespace trade_bot {

PaperExecutor::PaperExecutor(const std::map<Ticker, const OrderBook*>& books, Config cfg)
    : books_(books)
    , cfg_(cfg) {}

PaperExecutor::PaperExecutor(const std::map<Ticker, const OrderBook*>& books)
    : PaperExecutor(books, Config{}) {}

void PaperExecutor::submit(const TradePlan& plan) {
    pending_entries_.push_back(plan);
    LOG_INFO("PaperExecutor: submitted plan for {} (Entry: {}, Stop: {})", 
             plan.ticker, plan.entry_price, plan.stop_price);
}

void PaperExecutor::close_trade(const Ticker& ticker, const FixedString<32>& reason) {
    LOG_WARN("PaperExecutor: strategy-requested close for {}: {}", ticker, reason);
    std::lock_guard lock(mutex_);
    auto pit = positions_.find(ticker);
    if (pit == positions_.end()) return;

    auto& pos = pit->second;
    auto bit = books_.find(ticker);
    double exit_price = 0.0;
    if (bit != books_.end()) {
        const auto* book = bit->second;
        double bid = book->best_bid().value_or(0.0);
        double ask = book->best_ask().value_or(0.0);
        if (bid > 0.0 && ask > 0.0) {
            exit_price = (pos.plan.side == Side::Buy) ? bid : ask;
        }
    }
    if (exit_price == 0.0) {
        LOG_WARN("PaperExecutor: no book data for {} — using avg_price={} with slippage as exit (small loss)",
                 ticker, pos.avg_price);
        exit_price = pos.avg_price;  // fallback to entry (zero PnL)
    }

    // Apply slippage (consistent with SL/TP exits in tick())
    double slip = exit_price * (cfg_.slippage_bps / 10000.0);
    exit_price += (pos.plan.side == Side::Buy) ? -slip : slip;

    double pnl = (pos.plan.side == Side::Buy)
        ? (exit_price - pos.avg_price) * pos.executed_size
        : (pos.avg_price - exit_price) * pos.executed_size;

    LOG_INFO("PaperExecutor: closed {} {} at {:.4f} (slip={:.6f}) | PnL: {:.4f} | reason: {}",
             pos.plan.side == Side::Buy ? "Long" : "Short", ticker, exit_price, slip, pnl, reason);

    ClosedTrade ct;
    ct.plan = pos.plan;
    ct.entry_price = pos.avg_price;
    ct.exit_price = exit_price;
    ct.size_filled = pos.executed_size;
    ct.pnl_usd = pnl;
    ct.reason = reason;
    closed_trades_.push_back(ct);
    positions_.erase(pit);
}

void PaperExecutor::cancel_all(const Ticker& ticker) {
    pending_entries_.erase(
        std::remove_if(pending_entries_.begin(), pending_entries_.end(),
                       [&ticker](const TradePlan& p){ return p.ticker == ticker; }),
        pending_entries_.end());
    positions_.erase(ticker);
}

std::vector<ActiveTrade> PaperExecutor::get_active_trades() const {
    std::vector<ActiveTrade> res;
    for (const auto& [ticker, pos] : positions_) {
        ActiveTrade t;
        t.plan = pos.plan;
        t.executed_size = pos.executed_size;
        t.avg_entry_price = pos.avg_price;
        t.state = TradeState::Open;
        // Use exchange mark price when available (consistent with dashboard display),
        // fall back to (bid+ask)/2 mid if mark not yet received.
        double price = 0.0;
        auto mit = mark_prices_.find(ticker);
        if (mit != mark_prices_.end() && mit->second > 0.0) {
            price = mit->second;
        } else {
            auto bit = books_.find(ticker);
            if (bit != books_.end()) {
                double bid = bit->second->best_bid().value_or(0.0);
                double ask = bit->second->best_ask().value_or(0.0);
                if (bid > 0.0 && ask > 0.0) price = (bid + ask) * 0.5;
            }
        }
        if (price > 0.0) {
            t.unrealized_pnl = (pos.plan.side == Side::Buy)
                ? (price - pos.avg_price) * pos.executed_size
                : (pos.avg_price - price) * pos.executed_size;
        }
        res.push_back(t);
    }
    for (const auto& plan : pending_entries_) {
        ActiveTrade t;
        t.plan = plan;
        t.state = TradeState::PendingEntry;
        res.push_back(t);
    }
    return res;
}

void PaperExecutor::on_trade(const Ticker& ticker, const Trade& trade) {
    tick(ticker, trade.timestamp);
}

void PaperExecutor::on_orderbook_snapshot(const OrderBookSnapshot& snap) {
    tick(snap.ticker, snap.ts);
}

void PaperExecutor::on_orderbook_update(const OrderBookUpdate& upd) {
    tick(upd.ticker, upd.ts);
}

void PaperExecutor::tick(std::chrono::system_clock::time_point now) {
    // T4-PAPER: Collect all tickers that need processing (pending or open) (#157)
    std::vector<Ticker> active;
    active.reserve(pending_entries_.size() + positions_.size());
    for (const auto& p : pending_entries_) active.push_back(p.ticker);
    for (const auto& [t, _] : positions_) active.push_back(t);
    
    // De-duplicate
    std::sort(active.begin(), active.end());
    active.erase(std::unique(active.begin(), active.end()), active.end());

    for (const auto& ticker : active) {
        tick(ticker, now);
    }
}

void PaperExecutor::tick(const Ticker& ticker, std::chrono::system_clock::time_point now) {
    // 1. Handle Pending Entries for this ticker
    for (auto it = pending_entries_.begin(); it != pending_entries_.end(); ) {
        const auto& plan = *it;
        if (plan.ticker != ticker) { ++it; continue; }
        
        // TTL Check
        if (now > plan.valid_until) {
            LOG_INFO("PaperExecutor: plan for {} expired", plan.ticker);
            it = pending_entries_.erase(it);
            continue;
        }

        auto bit = books_.find(plan.ticker);
        if (bit == books_.end()) { ++it; continue; }
        
        const auto* book = bit->second;
        double bid = book->best_bid().value_or(0.0);
        double ask = book->best_ask().value_or(0.0);
        if (bid == 0.0 || ask == 0.0) { ++it; continue; }

        bool fill = false;
        double fill_price = 0.0;

        if (plan.entry_type == OrderType::Market) {
            fill = true;
            fill_price = (plan.side == Side::Buy) ? ask : bid;
        } else {
            // Limit fill
            if (plan.side == Side::Buy && ask <= plan.entry_price) {
                fill = true;
                fill_price = std::min(plan.entry_price, ask); // gap protection
            } else if (plan.side == Side::Sell && bid >= plan.entry_price) {
                fill = true;
                fill_price = std::max(plan.entry_price, bid); // gap protection
            }
        }

        if (fill) {
            // Apply slippage
            double slip = fill_price * (cfg_.slippage_bps / 10000.0);
            fill_price += (plan.side == Side::Buy) ? slip : -slip;

            LOG_INFO("PaperExecutor: FILLED Entry {} {} at {}",
                     plan.side == Side::Buy ? "Buy" : "Sell", plan.ticker, fill_price);

            // For market orders the fill price can deviate from plan.entry_price.
            // Shift TP and SL by the same offset so the trade's risk structure
            // is preserved relative to the actual fill price, preventing TP from
            // firing immediately at a loss when the market moved past TP before fill.
            TradePlan adjusted = plan;
            double price_shift = fill_price - plan.entry_price;
            if (price_shift != 0.0) {
                adjusted.entry_price  = fill_price;
                adjusted.stop_price  += price_shift;
                if (adjusted.tp1_price > 0.0) adjusted.tp1_price += price_shift;
                if (adjusted.tp2_price) *adjusted.tp2_price += price_shift;
            }

            positions_[plan.ticker] = {adjusted, plan.size_coin, fill_price};
            it = pending_entries_.erase(it);
        } else {
            ++it;
        }
    }

    // 2. Check Stops and TPs for existing positions of this ticker
    auto pit = positions_.find(ticker);
    if (pit != positions_.end()) {
        auto& pos = pit->second;
        auto bit = books_.find(pos.plan.ticker);
        if (bit != books_.end()) {
            const auto* book = bit->second;
            double bid = book->best_bid().value_or(0.0);
            double ask = book->best_ask().value_or(0.0);
            if (bid > 0.0 && ask > 0.0) {
                bool exit = false;
                std::string reason;
                double exit_price = 0.0;

                // Stop Loss check
                if (pos.plan.side == Side::Buy) {
                    if (bid <= pos.plan.stop_price) {
                        exit = true;
                        // Gap protection: if bid is below stop_price, fill at bid
                        exit_price = std::min(pos.plan.stop_price, bid);
                        reason = "Stop Loss";
                    }
                } else {
                    if (ask >= pos.plan.stop_price) {
                        exit = true;
                        // Gap protection: if ask is above stop_price, fill at ask
                        exit_price = std::max(pos.plan.stop_price, ask);
                        reason = "Stop Loss";
                    }
                }

                // Take Profit check (TP1 only for simplified PaperExecutor)
                if (!exit && pos.plan.tp1_price > 0.0) {
                    if (pos.plan.side == Side::Buy) {
                        if (ask >= pos.plan.tp1_price) {
                            exit = true;
                            // Gap protection: if ask is above tp_price, fill at ask (better fill)
                            exit_price = std::max(pos.plan.tp1_price, ask);
                            reason = "Take Profit";
                        }
                    } else {
                        if (bid <= pos.plan.tp1_price) {
                            exit = true;
                            // Gap protection: if bid is below tp_price, fill at bid (better fill)
                            exit_price = std::min(pos.plan.tp1_price, bid);
                            reason = "Take Profit";
                        }
                    }
                }

                if (exit) {
                    // Apply slippage on exit
                    double slip = exit_price * (cfg_.slippage_bps / 10000.0);
                    exit_price += (pos.plan.side == Side::Buy) ? -slip : slip;

                    double pnl = (pos.plan.side == Side::Buy)
                        ? (exit_price - pos.avg_price) * pos.executed_size
                        : (pos.avg_price - exit_price) * pos.executed_size;

                    LOG_INFO("PaperExecutor: FILLED Exit ({}) {} {} at {} | PnL: {:.4f}",
                             reason, pos.plan.side == Side::Buy ? "Sell" : "Buy",
                             pos.plan.ticker, exit_price, pnl);

                    {
                        std::lock_guard lock(mutex_);
                        ClosedTrade ct;
                        ct.plan = pos.plan;
                        ct.entry_price = pos.avg_price;
                        ct.exit_price = exit_price;
                        ct.size_filled = pos.executed_size;
                        ct.pnl_usd = pnl;
                        ct.reason = reason.c_str();
                        closed_trades_.push_back(ct);
                    }
                    positions_.erase(pit);
                }
            }
        }
    }
}

std::vector<IExecutor::ClosedTrade> PaperExecutor::pop_closed_trades() {
    std::lock_guard lock(mutex_);
    std::vector<ClosedTrade> result;
    result.swap(closed_trades_);
    return result;
}

double PaperExecutor::unrealized_pnl() const {
    double total = 0.0;
    for (const auto& [ticker, pos] : positions_) {
        double price = 0.0;
        auto mit = mark_prices_.find(ticker);
        if (mit != mark_prices_.end() && mit->second > 0.0) {
            price = mit->second;
        } else {
            auto bit = books_.find(ticker);
            if (bit == books_.end()) continue;
            double bid = bit->second->best_bid().value_or(0.0);
            double ask = bit->second->best_ask().value_or(0.0);
            if (bid == 0.0 || ask == 0.0) continue;
            price = (bid + ask) * 0.5;
        }
        total += (pos.plan.side == Side::Buy)
            ? (price - pos.avg_price) * pos.executed_size
            : (pos.avg_price - price) * pos.executed_size;
    }
    return total;
}

void PaperExecutor::inject_recovered_trades(const std::vector<ActiveTrade>& trades) {
    for (const auto& t : trades) {
        if (t.state == TradeState::Open || t.state == TradeState::Exiting) {
            positions_[t.plan.ticker] = Position{t.plan, t.executed_size, t.avg_entry_price};
            LOG_INFO("PaperExecutor: injected recovered position for {}", t.plan.ticker);
        }
    }
}

} // namespace trade_bot
