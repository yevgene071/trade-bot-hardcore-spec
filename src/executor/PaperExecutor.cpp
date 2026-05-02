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
    active_plans_.push_back(plan);
    LOG_INFO("PaperExecutor: submitted plan for {} (Entry: {}, Stop: {})", 
             plan.ticker, plan.entry_price, plan.stop_price);
}

void PaperExecutor::cancel_all(const Ticker& ticker) {
    active_plans_.erase(
        std::remove_if(active_plans_.begin(), active_plans_.end(),
                       [&ticker](const TradePlan& p){ return p.ticker == ticker; }),
        active_plans_.end());
}

void PaperExecutor::tick(std::chrono::system_clock::time_point now) {
    (void)now;
    for (auto it = active_plans_.begin(); it != active_plans_.end(); ) {
        const auto& plan = *it;
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

            LOG_INFO("PaperExecutor: FILLED {} {} at {}", 
                     plan.side == Side::Buy ? "Buy" : "Sell", plan.ticker, fill_price);
            
            positions_[plan.ticker] = {plan.ticker, plan.side, plan.size_coin, fill_price};
            it = active_plans_.erase(it);
        } else {
            ++it;
        }
    }

    // Check stops and TPs for existing positions
    for (auto it = positions_.begin(); it != positions_.end(); ) {
        auto& pos = it->second;
        auto bit = books_.find(pos.ticker);
        if (bit == books_.end()) { ++it; continue; }
        
        double bid = bit->second->best_bid().value_or(0.0);
        double ask = bit->second->best_ask().value_or(0.0);
        if (bid == 0.0 || ask == 0.0) { ++it; continue; }

        // This is simplified as we don't have the full plan linked to position yet 
        // in this stub. To do it properly, I'd need to store the active plan with the position.
        
        ++it;
    }
}

} // namespace trade_bot
