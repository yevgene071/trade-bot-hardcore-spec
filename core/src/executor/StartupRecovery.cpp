#include "StartupRecovery.hpp"
#include "logger/Logger.hpp"
#include "numeric/PriceUtils.hpp"
#include <cmath>
#include <algorithm>
#include <set>
#include <map>

namespace trade_bot {

StartupRecovery::StartupRecovery(int connection_id, IOrderGateway& gateway, AccountStatePersister& persister)
    : StartupRecovery(connection_id, gateway, persister, Config{}) {}

StartupRecovery::StartupRecovery(int connection_id, IOrderGateway& gateway, AccountStatePersister& persister, const Config& cfg)
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
                // Check for size mismatch drift (only if persisted size was recorded)
                double server_sz = std::abs(pit->size);
                double persisted_sz = ptit->plan.size_coin;
                if (persisted_sz > 0.0 &&
                    std::abs(server_sz - persisted_sz) > cfg_.position_drift_coin) {
                    drift_detected = true;
                    res.log_entries.push_back("Size drift for " + ticker +
                        ": server=" + std::to_string(server_sz) +
                        " persisted=" + std::to_string(persisted_sz));
                }
            } else {
                res.log_entries.push_back("Found orphan position for " + ticker + ", creating emergency trade");
                trade.plan.ticker = ticker;
                trade.plan.side = pit->side;
                trade.plan.entry_price = pit->avg_price;
                trade.plan.size_coin = std::abs(pit->size);
                
                // Emergency stop
                double stop_dist = pit->avg_price * cfg_.max_recovery_stop_bps / 10000.0;
                trade.plan.stop_price = pit->avg_price + (pit->side == Side::Buy ? -stop_dist : stop_dist);
                trade.plan.stop_price = round_to_tick(trade.plan.stop_price, pit->price_increment);
                
                // Fix for #131: Actually place the emergency stop on the exchange
                PlaceOrderRequest req;
                req.ticker = ticker;
                req.side = (pit->side == Side::Buy ? Side::Sell : Side::Buy); // Opposite to close
                req.type = OrderType::StopLoss;
                req.price = trade.plan.stop_price;
                req.size = trade.plan.size_coin;
                req.reduce_only = true;
                
                try {
                    auto sres = gateway_.place_order(connection_id_, req);
                    trade.stop_order_id = sres.order_id;
                    res.log_entries.push_back("Placed emergency stop for " + ticker + " at " + std::to_string(trade.plan.stop_price) + " (id=" + std::to_string(trade.stop_order_id) + ")");
                } catch (const std::exception& e) {
                    res.log_entries.push_back("ERROR: Failed to place emergency stop for " + ticker + ": " + e.what());
                }
                
                drift_detected = true;
            }
            trade.state           = TradeState::Open;
            trade.executed_size   = std::abs(pit->size);
            trade.avg_entry_price = pit->avg_price;
            // Populate avg_price_fix from server so BE-stop after TP1 works correctly.
            // Without this the field stays 0 and the BE-stop never fires for recovered trades.
            if (pit->avg_price_fix > 0.0)
                trade.avg_price_fix = pit->avg_price_fix;
            else
                trade.avg_price_fix = pit->avg_price;
            res.recovered_trades.push_back(trade);
            
            // Check if stop exists on the exchange.
            auto& orders = open_orders[ticker];
            bool has_stop = std::any_of(orders.begin(), orders.end(), [](const RestOrder& o) {
                return o.type == OrderType::StopLoss || o.type == OrderType::Stop;
            });

            if (!has_stop) {
                // trade.stop_order_id stays 0; LiveExecutor::tick() re-arms it after 5s.
                res.log_entries.push_back("WARNING: No stop-loss on exchange for recovered " +
                    ticker + " — will be re-armed by executor tick");
            }
        } else {
            // No position on server
            if (ptit != persisted.active_trades.end()) {
                res.log_entries.push_back("Persisted trade for " + ticker + " but no position on server. Closing locally.");
                drift_detected = true;
            }
        }
        
        // Handle orphan orders.
        // IMPORTANT: recovered trades often have order_id fields == 0 because
        // they were not persisted. Matching by ID alone would mark every SL/TP
        // as "orphan" and cancel them. Instead, for tickers with a recovered
        // position we first try to assign unmatched SL/TP orders to that trade
        // before falling back to the cancel policy.
        const auto& orders = open_orders[ticker];
        auto trade_it = std::find_if(res.recovered_trades.begin(), res.recovered_trades.end(),
            [&](const ActiveTrade& t) { return t.plan.ticker == ticker; });

        for (const auto& o : orders) {
            bool is_used = std::any_of(res.recovered_trades.begin(), res.recovered_trades.end(),
                [&](const ActiveTrade& t) {
                    return o.id != 0 && (o.id == t.entry_order_id ||
                                         o.id == t.stop_order_id  ||
                                         o.id == t.tp1_order_id   ||
                                         o.id == t.tp2_order_id);
                });

            if (!is_used && trade_it != res.recovered_trades.end()) {
                // Assign this order to the recovered trade by type instead of cancelling.
                if ((o.type == OrderType::StopLoss || o.type == OrderType::Stop)
                        && trade_it->stop_order_id == 0) {
                    trade_it->stop_order_id = o.id;
                    is_used = true;
                    res.log_entries.push_back("Assigned existing SL id=" + std::to_string(o.id) +
                                              " to recovered " + ticker);
                } else if (o.type == OrderType::TakeProfit && trade_it->tp1_order_id == 0) {
                    trade_it->tp1_order_id = o.id;
                    is_used = true;
                    res.log_entries.push_back("Assigned existing TP id=" + std::to_string(o.id) +
                                              " to recovered " + ticker);
                }
            }

            if (!is_used && cfg_.orphan_cancel_policy == "cancel") {
                res.log_entries.push_back("Cancelling orphan order " + std::to_string(o.id) + " for " + ticker);
                gateway_.cancel_order(connection_id_, o.id, ticker);
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
