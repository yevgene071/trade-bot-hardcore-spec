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

void PaperExecutor::tick(std::chrono::system_clock::time_point now) {
    // 1. Handle Pending Entries
    for (auto it = pending_entries_.begin(); it != pending_entries_.end(); ) {
        const auto& plan = *it;
        
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
                fill_price = plan.entry_price;
            } else if (plan.side == Side::Sell && bid >= plan.entry_price) {
                fill = true;
                fill_price = plan.entry_price;
            }
        }

        if (fill) {
            // Apply slippage
            double slip = fill_price * (cfg_.slippage_bps / 10000.0);
            fill_price += (plan.side == Side::Buy) ? slip : -slip;

            LOG_INFO("PaperExecutor: FILLED Entry {} {} at {}", 
                     plan.side == Side::Buy ? "Buy" : "Sell", plan.ticker, fill_price);
            
            positions_[plan.ticker] = {plan, plan.size_coin, fill_price};
            it = pending_entries_.erase(it);
        } else {
            ++it;
        }
    }

    // 2. Check Stops and TPs for existing positions
    for (auto it = positions_.begin(); it != positions_.end(); ) {
        auto& pos = it->second;
        auto bit = books_.find(pos.plan.ticker);
        if (bit == books_.end()) { ++it; continue; }
        
        const auto* book = bit->second;
        double bid = book->best_bid().value_or(0.0);
        double ask = book->best_ask().value_or(0.0);
        if (bid == 0.0 || ask == 0.0) { ++it; continue; }

        bool exit = false;
        std::string reason;
        double exit_price = 0.0;

        // Stop Loss check
        if (pos.plan.side == Side::Buy) {
            if (bid <= pos.plan.stop_price) {
                exit = true;
                exit_price = pos.plan.stop_price;
                reason = "Stop Loss";
            }
        } else {
            if (ask >= pos.plan.stop_price) {
                exit = true;
                exit_price = pos.plan.stop_price;
                reason = "Stop Loss";
            }
        }

        // Take Profit check (TP1 only for simplified PaperExecutor)
        if (!exit && pos.plan.tp1_price) {
            if (pos.plan.side == Side::Buy) {
                if (ask >= *pos.plan.tp1_price) {
                    exit = true;
                    exit_price = *pos.plan.tp1_price;
                    reason = "Take Profit";
                }
            } else {
                if (bid <= *pos.plan.tp1_price) {
                    exit = true;
                    exit_price = *pos.plan.tp1_price;
                    reason = "Take Profit";
                }
            }
        }

        if (exit) {
            // Apply slippage on exit
            double slip = exit_price * (cfg_.slippage_bps / 10000.0);
            exit_price += (pos.plan.side == Side::Buy) ? -slip : slip;

            LOG_INFO("PaperExecutor: FILLED Exit ({}) {} {} at {}", 
                     reason, pos.plan.side == Side::Buy ? "Sell" : "Buy", pos.plan.ticker, exit_price);
            
            it = positions_.erase(it);
        } else {
            ++it;
        }
    }
}

} // namespace trade_bot
