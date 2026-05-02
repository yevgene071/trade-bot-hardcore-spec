#include "LiveExecutor.hpp"
#include "logger/Logger.hpp"
#include <chrono>

namespace trade_bot {

LiveExecutor::LiveExecutor(int connection_id, IOrderGateway& gateway, MarketDataFeed& feed, Config cfg)
    : connection_id_(connection_id), gateway_(gateway), feed_(feed), cfg_(cfg) {
    
    reconciliator_.set_fetch_open_orders([this](const Ticker& ticker) {
        return gateway_.get_open_orders(connection_id_, ticker);
    });
    
    feed_.add_listener(this);
}

LiveExecutor::LiveExecutor(int connection_id, IOrderGateway& gateway, MarketDataFeed& feed)
    : LiveExecutor(connection_id, gateway, feed, Config{}) {}

void LiveExecutor::submit(const TradePlan& plan) {
    std::lock_guard lock(mutex_);
    
    ActiveTrade trade;
    trade.plan = plan;
    trade.state = TradeState::PendingEntry;
    
    PlaceOrderRequest req;
    req.ticker = plan.ticker;
    req.side = plan.side;
    req.price = plan.entry_price;
    req.size = plan.size_coin;
    req.type = plan.entry_type;

    try {
        auto res = gateway_.place_order(connection_id_, req);
        
        LOG_INFO("LiveExecutor: submitted entry for {} {} @ {}", 
                 plan.ticker, plan.side == Side::Buy ? "BUY" : "SELL", plan.entry_price);
        
        trades_[plan.ticker].push_back(trade);
        error_streak_ = 0;
        
    } catch (const std::exception& e) {
        LOG_ERROR("LiveExecutor: place_order error for {}: {}", plan.ticker, e.what());
        error_streak_++;
        
        OrderIntent intent{0, plan.ticker, plan.side, plan.entry_type, plan.entry_price, plan.size_coin};
        reconciliator_.enter_submit_unknown(intent);
        
        trade.state = TradeState::SubmitUnknown;
        trades_[plan.ticker].push_back(trade);
    }
}

void LiveExecutor::cancel_all(const Ticker& ticker) {
    std::lock_guard lock(mutex_);
    gateway_.cancel_all_orders(connection_id_, ticker);
    
    if (auto it = trades_.find(ticker); it != trades_.end()) {
        for (auto& t : it->second) {
            if (t.state != TradeState::Closed) {
                t.state = TradeState::Cancelling;
            }
        }
    }
}

void LiveExecutor::on_order_update(const OrderUpdate& upd) {
    std::lock_guard lock(mutex_);
    auto it = trades_.find(upd.ticker);
    if (it == trades_.end()) return;

    for (auto& trade : it->second) {
        if (trade.state == TradeState::Closed) continue;

        if (trade.plan.side == upd.side && std::abs(trade.plan.size_coin - upd.size) < 1e-6) {
            if (upd.status == OrderStatus::Open) {
                if (trade.state == TradeState::PendingEntry) {
                    // Entry acknowledged by server
                }
            } else if (upd.status == OrderStatus::Closed) {
                if (upd.filled_size > 0) {
                    if (trade.state == TradeState::PendingEntry || trade.state == TradeState::SubmitUnknown) {
                        // Entry filled
                        trade.state = TradeState::Open;
                        trade.executed_size = upd.filled_size;
                        trade.avg_entry_price = upd.filled_price;
                        trade.opened_at = upd.time;
                        
                        LOG_INFO("LiveExecutor: entry FILLED for {} @ {}", upd.ticker, upd.filled_price);
                        place_stops_(trade);
                    } else if (trade.state == TradeState::Exiting || trade.state == TradeState::Open) {
                        // Exit filled
                        trade.state = TradeState::Closed;
                        LOG_INFO("LiveExecutor: exit FILLED for {} @ {}", upd.ticker, upd.filled_price);
                    }
                } else {
                    // Cancelled
                    trade.state = TradeState::Closed;
                }
            }
        }
    }
}

void LiveExecutor::on_position_update([[maybe_unused]] const PositionUpdate& upd) {
    // Synchronize with server position if needed
}

void LiveExecutor::on_balance_update([[maybe_unused]] const BalanceUpdate& upd) {
    std::lock_guard lock(mutex_);
    last_balance_update_ = std::chrono::system_clock::now();
    reserved_balance_usd_ = 0.0;
}

void LiveExecutor::on_error(const std::string& msg) {
    LOG_ERROR("LiveExecutor: MarketDataFeed error: {}", msg);
}

void LiveExecutor::tick(std::chrono::system_clock::time_point now) {
    std::lock_guard lock(mutex_);
    
    // Check reconciliator
    // results = reconciliator_.poll_open_orders(...);
    
    if (reserved_balance_usd_ > 0 && (now - last_balance_update_) > cfg_.balance_reservation_timeout) {
        LOG_WARN("LiveExecutor: balance reservation TIMEOUT, releasing ${}", reserved_balance_usd_);
        reserved_balance_usd_ = 0.0;
    }
}

void LiveExecutor::place_stops_(ActiveTrade& trade) {
    // Place Stop Loss
    PlaceOrderRequest sl;
    sl.ticker = trade.plan.ticker;
    sl.side = trade.plan.side == Side::Buy ? Side::Sell : Side::Buy;
    sl.price = trade.plan.stop_price;
    sl.size = trade.executed_size;
    sl.type = OrderType::StopLoss;
    sl.reduce_only = true;
    
    try {
        gateway_.place_order(connection_id_, sl);
        LOG_INFO("LiveExecutor: placed StopLoss for {} @ {}", sl.ticker, sl.price);
    } catch (const std::exception& e) {
        LOG_ERROR("LiveExecutor: failed to place SL for {}: {}", sl.ticker, e.what());
    }

    // Place TP1 if exists
    if (trade.plan.tp1_price > 0) {
        PlaceOrderRequest tp;
        tp.ticker = trade.plan.ticker;
        tp.side = sl.side;
        tp.price = trade.plan.tp1_price;
        tp.size = trade.executed_size * trade.plan.tp1_size_ratio;
        tp.type = OrderType::TakeProfit;
        tp.reduce_only = true;
        
        try {
            gateway_.place_order(connection_id_, tp);
            LOG_INFO("LiveExecutor: placed TP1 for {} @ {}", tp.ticker, tp.price);
        } catch (const std::exception& e) {
             LOG_ERROR("LiveExecutor: failed to place TP1 for {}: {}", tp.ticker, e.what());
        }
    }
}

} // namespace trade_bot
