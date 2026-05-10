#include "LiveExecutor.hpp"
#include "logger/Logger.hpp"
#include "metrics/MetricsRegistry.hpp"
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
    
    // T4-RISK: Reserve balance for the pending order
    double reservation = plan.size_coin * plan.entry_price;
    reserved_balance_usd_ += reservation;
    LOG_DEBUG("LiveExecutor: reserved ${} for {}, total reserved: ${}", 
              reservation, plan.ticker, reserved_balance_usd_);

    PlaceOrderRequest req;
    req.ticker = plan.ticker;
    req.side = plan.side;
    req.price = plan.entry_price;
    req.size = plan.size_coin;
    req.type = plan.entry_type;

    try {
        auto start = std::chrono::steady_clock::now();
        auto res = gateway_.place_order(connection_id_, req);
        auto end = std::chrono::steady_clock::now();
        double lat_ms = std::chrono::duration<double, std::milli>(end - start).count();
        MetricsRegistry::instance().histogram_observe("trade_bot_order_latency_ms", lat_ms);

        LOG_INFO("LiveExecutor: submitted entry for {} {} @ {}", 
                 plan.ticker, plan.side == Side::Buy ? "BUY" : "SELL", plan.entry_price);
        
        trades_[plan.ticker].push_back(trade);
        error_streak_ = 0;
        
    } catch (const std::exception& e) {
        LOG_ERROR("LiveExecutor: place_order error for {}: {}", plan.ticker, e.what());
        int streak = ++error_streak_;
        if (streak >= cfg_.exchange_error_streak_limit) {
            if (alert_cb_) alert_cb_("Exchange error streak hit: " + std::to_string(streak) + " errors in a row.");
        }
        
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
                // If it was pending, release reservation (optimistic)
                if (t.state == TradeState::PendingEntry) {
                    double reservation = t.plan.size_coin * t.plan.entry_price;
                    reserved_balance_usd_ = std::max(0.0, reserved_balance_usd_ - reservation);
                }
                t.state = TradeState::Cancelling;
            }
        }
    }
}

void LiveExecutor::inject_recovered_trades(const std::vector<ActiveTrade>& trades) {
    std::lock_guard lock(mutex_);
    for (const auto& t : trades) {
        trades_[t.plan.ticker].push_back(t);
        LOG_INFO("LiveExecutor: injected recovered trade for {} (state={})", 
                 t.plan.ticker, static_cast<int>(t.state));
    }
}

std::vector<ActiveTrade> LiveExecutor::get_active_trades() const {
    std::lock_guard lock(mutex_);
    std::vector<ActiveTrade> out;
    for (const auto& [ticker, list] : trades_) {
        for (const auto& t : list) {
            // T4-RISK: Compute unrealized PnL for dashboard/metrics
            ActiveTrade copy = t;
            auto mit = mark_prices_.find(ticker);
            if (mit != mark_prices_.end() && t.state == TradeState::Open) {
                double mark = mit->second;
                if (t.plan.side == Side::Buy) {
                    copy.unrealized_pnl = (mark - t.avg_entry_price) * t.executed_size;
                } else {
                    copy.unrealized_pnl = (t.avg_entry_price - mark) * t.executed_size;
                }
            }
            out.push_back(copy);
        }
    }
    return out;
}

std::vector<IExecutor::ClosedTrade> LiveExecutor::pop_closed_trades() {
    std::lock_guard lock(mutex_);
    std::vector<ClosedTrade> out;
    out.swap(closed_trades_);
    return out;
}

void LiveExecutor::set_mark_prices(const std::unordered_map<Ticker, double>& marks) {
    std::lock_guard lock(mutex_);
    mark_prices_ = marks;
}

void LiveExecutor::on_order_update(const OrderUpdate& upd) {
    std::lock_guard lock(mutex_);
    auto it = trades_.find(upd.ticker);
    if (it == trades_.end()) return;

    constexpr double kSizeEps = 1e-6;
    const bool is_entry_type =
        upd.type == OrderType::Limit || upd.type == OrderType::Market ||
        upd.type == OrderType::Stop;
    const bool is_stop_type = upd.type == OrderType::StopLoss;
    const bool is_tp_type   = upd.type == OrderType::TakeProfit;

    for (auto& trade : it->second) {
        if (trade.state == TradeState::Closed) continue;

        // ---- Phase 1: route by known order_id (#129). Once we've seen
        // the first WS update for an order, we can match it deterministically
        // on subsequent fills.
        const bool match_by_entry = upd.order_id != 0 && upd.order_id == trade.entry_order_id;
        const bool match_by_stop  = upd.order_id != 0 && upd.order_id == trade.stop_order_id;
        const bool match_by_tp1   = upd.order_id != 0 && upd.order_id == trade.tp1_order_id;
        const bool match_by_tp2   = upd.order_id != 0 && upd.order_id == trade.tp2_order_id;

        // ---- Phase 2: first-time match — server tells us OrderId only via
        // WS, so we have to claim the id on first sight by side+type+size.
        const bool side_eq        = upd.side == trade.plan.side;
        const bool size_close     = std::abs(trade.plan.size_coin - upd.size) < kSizeEps;

        const bool fresh_entry =
            !match_by_entry && !match_by_stop && !match_by_tp1 && !match_by_tp2 &&
            trade.entry_order_id == 0 && is_entry_type && side_eq && size_close;
        const bool fresh_stop =
            !match_by_entry && !match_by_stop && !match_by_tp1 && !match_by_tp2 &&
            trade.stop_order_id == 0 && is_stop_type && upd.side != trade.plan.side;
        const bool fresh_tp1 =
            !match_by_entry && !match_by_stop && !match_by_tp1 && !match_by_tp2 &&
            trade.tp1_order_id == 0 && is_tp_type && upd.side != trade.plan.side;

        if (fresh_entry) trade.entry_order_id = upd.order_id;
        if (fresh_stop)  trade.stop_order_id  = upd.order_id;
        if (fresh_tp1)   trade.tp1_order_id   = upd.order_id;

        const bool is_entry = match_by_entry || fresh_entry;
        const bool is_stop  = match_by_stop  || fresh_stop;
        const bool is_tp1   = match_by_tp1   || fresh_tp1;
        const bool is_tp2   = match_by_tp2;

        if (!is_entry && !is_stop && !is_tp1 && !is_tp2) {
            continue;       // unrelated event for this trade
        }

        // FOUND THE MATCHING TRADE — Process it and stop searching (#129)
        if (upd.status == OrderStatus::Open) {
            return; // acknowledged, id captured above
        }
        
        // T4-RISK: Release reservation when order is either filled or cancelled
        if (is_entry && (upd.status == OrderStatus::Closed || upd.filled_size > 0)) {
            // If we already filled some, we release the WHOLE reservation and
            // let the actual balance update from exchange take over.
            double reservation = trade.plan.size_coin * trade.plan.entry_price;
            reserved_balance_usd_ = std::max(0.0, reserved_balance_usd_ - reservation);
        }

        if (upd.status != OrderStatus::Closed) {
            return;
        }

        if (upd.filled_size <= 0) {
            // Cancelled before fill (or zero-fill close).
            if (is_stop || is_tp1 || is_tp2) {
                LOG_INFO("LiveExecutor: exit order id={} on {} cancelled before fill",
                         upd.order_id, upd.ticker);
            } else {
                trade.state = TradeState::Closed;
            }
            return;
        }

        if (is_entry) {
            trade.state           = TradeState::Open;
            trade.executed_size   = upd.filled_size;
            trade.avg_entry_price = upd.filled_price;
            trade.opened_at       = upd.time;
            LOG_INFO("LiveExecutor: entry FILLED for {} @ {}",
                     upd.ticker, upd.filled_price);
            MetricsRegistry::instance().counter_inc(
                "trade_bot_trades_total",
                {{"ticker", upd.ticker}, {"strategy", trade.plan.strategy_name}});
            place_stops_(trade);
        } else if (is_tp1 && !trade.tp1_filled) {
            // T4-EXECUTOR spec: on TP1 fill, cancel current SL and re-place
            // it at AvgPriceFix (BE-stop). Issue #126.
            trade.tp1_filled = true;
            LOG_INFO("LiveExecutor: TP1 FILLED for {} @ {} — re-arming SL at AvgPriceFix={}",
                     upd.ticker, upd.filled_price, trade.avg_price_fix);
            if (trade.stop_order_id != 0) {
                try {
                    gateway_.cancel_order(connection_id_, trade.stop_order_id);
                } catch (const std::exception& e) {
                    LOG_WARN("LiveExecutor: cancel old SL id={} failed: {}",
                             trade.stop_order_id, e.what());
                }
                trade.stop_order_id = 0;   // re-claim on next stop order_update
            }
            const double remaining = std::max(0.0, trade.executed_size - upd.filled_size);
            if (remaining > kSizeEps && trade.avg_price_fix > 0.0) {
                PlaceOrderRequest be{};
                be.ticker      = trade.plan.ticker;
                be.side        = trade.plan.side == Side::Buy ? Side::Sell : Side::Buy;
                be.price       = trade.avg_price_fix;
                be.size        = remaining;
                be.type        = OrderType::StopLoss;
                be.reduce_only = true;
                try {
                    gateway_.place_order(connection_id_, be);
                } catch (const std::exception& e) {
                    LOG_ERROR("LiveExecutor: BE-stop place failed for {}: {}",
                              trade.plan.ticker, e.what());
                }
            }
        } else if (is_stop || is_tp2 || (is_tp1 && trade.tp1_filled)) {
            trade.state = TradeState::Closed;
            LOG_INFO("LiveExecutor: exit FILLED for {} @ {} (id={})",
                     upd.ticker, upd.filled_price, upd.order_id);
            
            // T4-EXECUTOR: Record closed trade for journal/equity updates
            ClosedTrade ct;
            ct.plan = trade.plan;
            ct.entry_price = trade.avg_entry_price;
            ct.exit_price = upd.filled_price;
            ct.size_filled = upd.filled_size;
            if (trade.plan.side == Side::Buy) {
                ct.pnl_usd = (ct.exit_price - ct.entry_price) * ct.size_filled;
            } else {
                ct.pnl_usd = (ct.entry_price - ct.exit_price) * ct.size_filled;
            }
            ct.reason = is_stop ? "StopLoss" : (is_tp2 ? "TP2" : "TP1");
            closed_trades_.push_back(ct);
        }
        return; // Done with this update
    }
}
}

void LiveExecutor::on_position_update(const PositionUpdate& upd) {
    // Capture AvgPriceFix per ticker so the BE-stop after TP1 prices
    // against entry-only weighted average. Issue #126.
    std::lock_guard lock(mutex_);
    auto it = trades_.find(upd.ticker);
    if (it == trades_.end()) return;
    for (auto& trade : it->second) {
        if (trade.state == TradeState::Closed) continue;
        if (trade.plan.side == upd.side) {
            trade.avg_price_fix = upd.avg_price_fix;
        }
    }
}

void LiveExecutor::on_balance_update([[maybe_unused]] const BalanceUpdate& upd) {
    // README OQ-4: a BalanceUpdate doesn't carry an OrderId or reservation
    // id, so we can't match it to a specific outstanding place_order. We
    // record the timestamp so tick() can release stale reservations after
    // `balance_reservation_timeout`, but we do NOT zero reserved_balance_usd_
    // on every event — that previously wiped the reservation on any
    // unrelated balance tick (#129).
    std::lock_guard lock(mutex_);
    last_balance_update_ = std::chrono::system_clock::now();
}

void LiveExecutor::on_error(const std::string& msg) {
    LOG_ERROR("LiveExecutor: MarketDataFeed error: {}", msg);
}

void LiveExecutor::tick(std::chrono::system_clock::time_point now) {
    // Snapshot the active ticker set under the lock — release before any
    // calls into reconciliator (which calls back into the gateway via the
    // injected fetcher and would otherwise contend with order_update).
    std::vector<Ticker> tickers_snap;
    {
        std::lock_guard lock(mutex_);
        if (reserved_balance_usd_ > 0 &&
            (now - last_balance_update_) > cfg_.balance_reservation_timeout) {
            LOG_WARN("LiveExecutor: balance reservation TIMEOUT, releasing ${}",
                     reserved_balance_usd_);
            reserved_balance_usd_ = 0.0;
        }
        tickers_snap.reserve(trades_.size());
        for (const auto& [t, _] : trades_) tickers_snap.push_back(t);
    }

    // Drive the reconciliator. Without this, any trade that landed in
    // SubmitUnknown stays there forever — the entire point of
    // T0-ORDER-RECONCILIATION. Issue #129.
    for (const auto& ticker : tickers_snap) {
        if (!reconciliator_.has_pending(ticker)) {
            // T4-RECOVERY: For recovered trades that are Open but missing stops, re-arm them.
            std::lock_guard lock(mutex_);
            auto it = trades_.find(ticker);
            if (it != trades_.end()) {
                for (auto& trade : it->second) {
                    if (trade.state == TradeState::Open && 
                        trade.stop_order_id == 0 && 
                        (now - trade.opened_at) > std::chrono::seconds(5)) {
                        LOG_WARN("LiveExecutor: recovered trade for {} is Open but missing SL, re-arming", ticker);
                        place_stops_(trade);
                    }
                }
            }
            continue;
        }
        const auto results = reconciliator_.poll_open_orders(ticker);
        for (const auto& res : results) {
            handle_reconciled_(res);
        }
    }
}

void LiveExecutor::handle_reconciled_(const ReconcileResult& res) {
    if (res.outcome == ReconcileOutcome::Pending) return;

    std::lock_guard lock(mutex_);
    for (auto& [ticker, trades] : trades_) {
        for (auto& trade : trades) {
            if (trade.state != TradeState::SubmitUnknown) continue;
            // The reconciliator key is the local intent's local_order_id,
            // which we stored as 0 (unknown) in the SubmitUnknown trade.
            // Match by ticker+side+price+size; OrderReconciliator already
            // honours per-side / per-type tolerances when finding the
            // server order, so reaching this point implies the trade
            // belongs to this result.
            if (res.outcome == ReconcileOutcome::Resolved) {
                if (res.server_order_id) trade.entry_order_id = *res.server_order_id;
                trade.state = TradeState::PendingEntry;
                LOG_INFO("LiveExecutor: SubmitUnknown resolved → entry_order_id={} on {}",
                         trade.entry_order_id, ticker);
            } else if (res.outcome == ReconcileOutcome::NotFoundTimeout) {
                LOG_ERROR("LiveExecutor: SubmitUnknown TIMEOUT on {} — operator action required",
                          ticker);
                if (alert_cb_) {
                    alert_cb_("SubmitUnknown timed out on " + ticker +
                              " — manual ack needed");
                }
                trade.state = TradeState::Closed;
            }
            return;   // one result per call
        }
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
        auto res = gateway_.place_order(connection_id_, sl);
        trade.stop_order_id = res.order_id;
        LOG_INFO("LiveExecutor: placed StopLoss for {} @ {} (id={})", sl.ticker, sl.price, trade.stop_order_id);
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
        
        if (tp.size > 0) {
            try {
                auto res = gateway_.place_order(connection_id_, tp);
                trade.tp1_order_id = res.order_id;
                LOG_INFO("LiveExecutor: placed TP1 for {} @ {} (id={})", tp.ticker, tp.price, trade.tp1_order_id);
            } catch (const std::exception& e) {
                 LOG_ERROR("LiveExecutor: failed to place TP1 for {}: {}", tp.ticker, e.what());
            }
        } else {
            LOG_WARN("LiveExecutor: TP1 size is zero for {} (executed_size={}), skipping", 
                     tp.ticker, trade.executed_size);
        }
    }
}

} // namespace trade_bot
