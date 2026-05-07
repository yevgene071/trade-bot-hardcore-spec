#include "StartupRecovery.hpp"
#include "logger/Logger.hpp"
#include <cmath>
#include <algorithm>
#include <set>
#include <map>

namespace trade_bot {

StartupRecovery::StartupRecovery(int connection_id, IOrderGateway& gateway, AccountStatePersister& persister, Config cfg)
    : connection_id_(connection_id), gateway_(gateway), persister_(persister), cfg_(cfg) {}

StartupRecovery::Result StartupRecovery::run() {
    Result res;
    res.auto_ack = cfg_.auto_ack_on_clean;

    auto persisted_opt = persister_.load();
    AccountStatePersister::PersistedData persisted;
    if (persisted_opt) {
        persisted = *persisted_opt;
    } else {
        res.log_entries.push_back("No persisted state found, checking for orphans");
    }

    auto positions = gateway_.get_positions(connection_id_);
    
    // For each ticker in persisted active_trades or positions
    std::set<Ticker> tickers;
    for (const auto& t : persisted.active_trades) tickers.insert(t.plan.ticker);
    for (const auto& p : positions) tickers.insert(p.ticker);

    std::map<Ticker, std::vector<RestOrder>> open_orders;
    for (const auto& ticker : tickers) {
        open_orders[ticker] = gateway_.get_open_orders(connection_id_, ticker);
    }

    bool drift_detected = false;

    for (const auto& ticker : tickers) {
        // Find position
        auto pit = std::find_if(positions.begin(), positions.end(), [&](const PositionUpdate& p) {
            return p.ticker == ticker && std::abs(p.size) > cfg_.position_drift_coin;
        });

        // Find persisted trade
        auto ptit = std::find_if(persisted.active_trades.begin(), persisted.active_trades.end(), [&](const ActiveTrade& t) {
            return t.plan.ticker == ticker;
        });

        if (pit != positions.end()) {
            // Position on server
            ActiveTrade trade;
            if (ptit != persisted.active_trades.end()) {
                trade = *ptit;
                res.log_entries.push_back("Recovering active trade for " + ticker);
            } else {
                res.log_entries.push_back("Found orphan position for " + ticker + ", creating emergency trade");
                trade.plan.ticker = ticker;
                trade.plan.side = pit->side;
                trade.plan.entry_price = pit->avg_price;
                trade.plan.size_coin = std::abs(pit->size);
                
                // Emergency stop
                double stop_dist = pit->avg_price * cfg_.max_recovery_stop_bps / 10000.0;
                trade.plan.stop_price = pit->avg_price + (pit->side == Side::Buy ? -stop_dist : stop_dist);
                
                // Fix for #131: Actually place the emergency stop on the exchange
                PlaceOrderRequest req;
                req.ticker = ticker;
                req.side = (pit->side == Side::Buy ? Side::Sell : Side::Buy); // Opposite to close
                req.type = OrderType::Stop;
                req.price = trade.plan.stop_price;
                req.size = trade.plan.size_coin;
                
                auto resp = gateway_.place_order(connection_id_, req);
                if (resp.success) {
                    res.log_entries.push_back("Placed emergency stop for " + ticker + " at " + std::to_string(trade.plan.stop_price));
                } else {
                    res.log_entries.push_back("ERROR: Failed to place emergency stop for " + ticker + ": " + resp.error_msg);
                }
                
                drift_detected = true;
            }
            trade.state = TradeState::Open;
            trade.executed_size = std::abs(pit->size);
            trade.avg_entry_price = pit->avg_price;
            res.recovered_trades.push_back(trade);
            
            // Check if stop exists
            auto& orders = open_orders[ticker];
            bool has_stop = std::any_of(orders.begin(), orders.end(), [](const RestOrder& o) {
                return o.type == OrderType::StopLoss || o.type == OrderType::Stop;
            });
            
            if (!has_stop) {
                res.log_entries.push_back("WARNING: No stop-loss found for recovered " + ticker);
            }
        } else {
            // No position on server
            if (ptit != persisted.active_trades.end()) {
                res.log_entries.push_back("Persisted trade for " + ticker + " but no position on server. Closing locally.");
                drift_detected = true;
            }
        }
        
        // Handle orphan orders
        auto& orders = open_orders[ticker];
        for (const auto& o : orders) {
            bool is_used = std::any_of(res.recovered_trades.begin(), res.recovered_trades.end(), [&](const ActiveTrade& t) {
                return t.plan.ticker == ticker;
            });
            
            if (!is_used && cfg_.orphan_cancel_policy == "cancel") {
                res.log_entries.push_back("Cancelling orphan order " + std::to_string(o.id) + " for " + ticker);
                gateway_.cancel_order(connection_id_, o.id);
            }
        }
    }

    if (drift_detected) {
        res.auto_ack = false;
        res.log_entries.push_back("Significant drift detected between persisted and server state. Manual ack required.");
    }

    return res;
}

} // namespace trade_bot
