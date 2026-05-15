#include "app/BotApp.hpp"

#include "control/KillSwitch.hpp"
#include "control/TickerController.hpp"
#include "executor/IExecutor.hpp"
#include "logger/Logger.hpp"
#include "logger/TradeJournal.hpp"
#include "marketdata/OrderBook.hpp"
#include "metrics/MetricsRegistry.hpp"
#include "risk/AccountStatePersister.hpp"
#include "risk/RiskManager.hpp"
#include "risk/TradingDay.hpp"
#include "strategy/StrategyEngine.hpp"
#include "transport/FinresHandler.hpp"
#include "transport/MarketDataFeed.hpp"
#include "universe/TickerUniverse.hpp"

#include <spdlog/fmt/ranges.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

namespace trade_bot {

// ═══════════════════════════════════════════════════════════════
// Tick / Runtime
// ═══════════════════════════════════════════════════════════════

void BotApp::schedule_tick() {
    timer_.expires_after(std::chrono::milliseconds(100));
    timer_.async_wait([this](const boost::system::error_code& ec) {
        if (ec) return;
        if (!kill_switch_->is_triggered()) {
            tick();
            schedule_tick();
        } else {
            handle_kill_switch_();
        }
    });
}

std::string BotApp::normalize_ticker_(const std::string& name) {
    if (name.find('_') == std::string::npos && name.size() > 4)
        return name.substr(0, name.size() - 4) + "_" + name.substr(name.size() - 4);
    return name;
}

void BotApp::update_account_from_exchange_() {
    if (!live_executor_ || !finres_handler_) return;
    auto finres = finres_handler_->get_snapshot(m_connection_id_for_tick);
    if (finres && finres->equity > 0.0) {
        account_state_.free_balance_usd = finres->balance;
        account_state_.equity_usd       = finres->equity;
    }
}

void BotApp::check_daily_reset_() {
    if (!TradingDay::is_new_day(last_reset_day_)) return;
    account_state_.starting_equity_usd    = account_state_.equity_usd;
    account_state_.realized_pnl_today_usd = 0.0;
    last_reset_day_ = TradingDay::current_date_utc();
}

void BotApp::tick_controllers_(std::chrono::system_clock::time_point now,
                               const std::string& leader_ticker) {
    std::optional<FeatureFrame> leader_frame;
    if (auto it = controllers_.find(leader_ticker); it != controllers_.end()) {
        auto funding = feed_->get_funding(leader_ticker);
        if (funding && last_funding_times_[leader_ticker] != funding->next_funding_time) {
            risk_manager_->update_funding_time(leader_ticker, funding->next_funding_time);
            last_funding_times_[leader_ticker] = funding->next_funding_time;
        }
        leader_frame = it->second->tick(now);
        strategy_engine_->on_frame(*leader_frame);

        // GAP-4: update stats cache for leader ticker
        auto& lstats = ticker_stats_cache_[leader_ticker];
        if (funding) lstats.funding_rate_bps = funding->rate * 10000.0;
        if (it->second->book) {
            auto bid = it->second->book->best_bid();
            auto ask = it->second->book->best_ask();
            if (bid && ask && *bid > 0.0)
                lstats.avg_spread_bps = (*ask - *bid) / *bid * 10000.0;
        }
        lstats.volatility_1min_bps = leader_frame->volatility_1min_bps;
    }
    for (auto& [ticker, ctrl] : controllers_) {
        if (ticker == leader_ticker) continue;
        auto funding = feed_->get_funding(ticker);
        if (funding && last_funding_times_[ticker] != funding->next_funding_time) {
            risk_manager_->update_funding_time(ticker, funding->next_funding_time);
            last_funding_times_[ticker] = funding->next_funding_time;
        }
        if (leader_frame) ctrl->on_leader_frame(*leader_frame);
        auto frame = ctrl->tick(now);
        strategy_engine_->on_frame(frame);

        // GAP-4: update stats cache for this ticker
        auto& stats = ticker_stats_cache_[ticker];
        if (funding) stats.funding_rate_bps = funding->rate * 10000.0;
        if (ctrl->book) {
            auto bid = ctrl->book->best_bid();
            auto ask = ctrl->book->best_ask();
            if (bid && ask && *bid > 0.0)
                stats.avg_spread_bps = (*ask - *bid) / *bid * 10000.0;
        }
        stats.volatility_1min_bps = frame.volatility_1min_bps;
    }
}

bool BotApp::process_closed_trades_(std::chrono::system_clock::time_point now) {
    bool any_closed = false;
    for (const auto& ct : executor_->pop_closed_trades()) {
        any_closed = true;
        account_state_.realized_pnl_today_usd += ct.pnl_usd;
        account_state_.equity_usd             += ct.pnl_usd;
        account_state_.free_balance_usd       += ct.pnl_usd;
        risk_manager_->record_trade_end(ct.pnl_usd < 0, now);
        cached_consecutive_losses_ = (ct.pnl_usd < 0) ? cached_consecutive_losses_ + 1 : 0;
        TradeJournal::Entry je;
        je.plan          = ct.plan;
        je.pnl_usd       = ct.pnl_usd;
        je.exit_price    = ct.exit_price;
        je.cause_of_exit = ct.reason;
        je.ts_unix_ms    = std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()).count();
        journal_.log_entry(je);
    }
    return any_closed;
}

void BotApp::rebuild_slow_dash_cache_(std::chrono::system_clock::time_point now) {
    LOG_INFO("Dashboard update: journal={}, signals={}, universe={}, sessions={}",
             journal_.size(), recent_signals_.size(), universe_.active().size(),
             dashboard_->session_count());
    cached_journal_  = journal_.get_recent_entries(20);
    cached_signals_.assign(recent_signals_.begin(), recent_signals_.end());
    cached_sig_counts_ = signal_counts_;
    cached_universe_.clear();
    for (const auto& ticker : universe_.active()) {
        DashboardServer::State::UniverseRow row;
        row.ticker     = ticker;
        row.strategies = universe_.enabled_strategies(ticker);
        row.boosted    = universe_.is_boosted(ticker, now);
        cached_universe_.push_back(std::move(row));
    }
    cached_version_ = Config::get<std::string>("app.version");

    cached_strategy_stats_.clear();
    {
        auto all_entries = journal_.get_recent_entries(200);
        std::map<std::string, DashboardServer::State::StrategyStats> by_strat;
        for (const auto& e : all_entries) {
            auto& st = by_strat[std::string(e.plan.strategy_name)];
            st.name = e.plan.strategy_name;
            st.total_trades++;
            st.total_pnl += e.pnl_usd;
            if (e.pnl_usd > 0) {
                st.wins++;
                st.gross_profit += e.pnl_usd;
            } else if (e.pnl_usd < 0) {
                st.losses++;
                st.gross_loss += std::abs(e.pnl_usd);
            }
            if (e.pnl_usd > st.best_pnl || st.total_trades == 1) st.best_pnl = e.pnl_usd;
            if (e.pnl_usd < st.worst_pnl || st.total_trades == 1) st.worst_pnl = e.pnl_usd;
        }
        cached_strategy_stats_.reserve(by_strat.size());
        for (auto& [_, st] : by_strat) cached_strategy_stats_.push_back(std::move(st));
    }

    cached_funding_info_.clear();
    for (const auto& ticker : universe_.active()) {
        auto fi = feed_->get_funding(ticker);
        if (!fi) continue;
        DashboardServer::State::FundingInfo info;
        info.ticker = ticker;
        info.rate   = fi->rate;
        info.next_funding_unix = static_cast<int64_t>(
            std::chrono::duration_cast<std::chrono::seconds>(
                fi->next_funding_time.time_since_epoch()).count());
        cached_funding_info_.push_back(std::move(info));
    }
    last_slow_dash_update_ = now;
}

void BotApp::send_dashboard_state_(std::chrono::system_clock::time_point now,
                                   const std::vector<ActiveTrade>& active,
                                   double upnl_sum,
                                   const std::string& leader_ticker,
                                   const std::unordered_map<Ticker, double>& all_marks) {
    DashboardServer::State dash_state;
    dash_state.account            = account_state_;
    dash_state.open_trades        = active;
    dash_state.kill_switch_active = kill_switch_->is_triggered();
    dash_state.version            = cached_version_;
    dash_state.signal_counts      = cached_sig_counts_;
    dash_state.recent_journal     = cached_journal_;
    dash_state.recent_signals     = cached_signals_;
    dash_state.metascalp.connected       = feed_->is_connected();
    dash_state.metascalp.connection_name = "MetaScalp-Local";
    dash_state.universe_rows   = cached_universe_;
    dash_state.strategy_stats  = cached_strategy_stats_;
    dash_state.funding_info    = cached_funding_info_;
    dash_state.recorder_active = dump_recorder_.is_active();
    dash_state.recorder_path   = dump_recorder_.path();
    dash_state.server_time_unix = std::chrono::duration_cast<std::chrono::seconds>(
        now.time_since_epoch()).count();

    for (auto& row : dash_state.universe_rows) {
        auto mit = all_marks.find(row.ticker);
        if (mit != all_marks.end()) row.mark_price = mit->second;
    }

    const double eq = account_state_.equity_usd;
    const double start_eq = account_state_.starting_equity_usd;
    dash_state.risk.total_trades_today   = static_cast<int>(cached_journal_.size());
    dash_state.risk.consecutive_losses   = cached_consecutive_losses_;
    dash_state.risk.margin_used_pct      = eq > 0 ? ((eq - account_state_.free_balance_usd) / eq) * 100.0 : 0.0;
    dash_state.risk.exposure_pct         = (eq > 0 && !active.empty()) ? (upnl_sum / eq) * 100.0 : 0.0;
    dash_state.risk.daily_pnl_pct        = start_eq > 0 ? (account_state_.realized_pnl_today_usd / start_eq) * 100.0 : 0.0;
    dash_state.risk.current_drawdown_pct = start_eq > 0 ? ((eq - start_eq) / start_eq) * 100.0 : 0.0;
    dash_state.risk.max_positions        = static_cast<int>(Config::get_or<int64_t>("risk.max_concurrent_positions", 3));

    dash_state.strategy_states = strategy_engine_->get_all_states();

    std::string sel_ticker = dashboard_->get_selected_ticker();
    if (sel_ticker.empty()) sel_ticker = leader_ticker;
    dash_state.selected_ticker = sel_ticker;

    if (auto it = controllers_.find(sel_ticker); it != controllers_.end()) {
        dash_state.chart_history = it->second->chart_snapshot();
        auto [bids, asks] = it->second->ob_snapshot(20);
        dash_state.bids_top20 = std::move(bids);
        dash_state.asks_top20 = std::move(asks);
        if (it->second->book) {
            auto best_bid = it->second->book->best_bid();
            auto best_ask = it->second->book->best_ask();
            if (best_bid && best_ask) {
                dash_state.ob_mid        = 0.5 * (*best_bid + *best_ask);
                dash_state.ob_spread_bps = (*best_ask - *best_bid) / *best_bid * kBpsBase;
                double bid_d  = it->second->book->bid_depth(10);
                double ask_d  = it->second->book->ask_depth(10);
                double total  = bid_d + ask_d;
                dash_state.ob_imbalance = (total > 0) ? (bid_d - ask_d) / total : 0.0;
            }
        }
    }
    dashboard_->update_state_async(std::move(dash_state));
}

void BotApp::tick() {
    auto now = std::chrono::system_clock::now();

    update_account_from_exchange_();
    check_daily_reset_();

    const std::string leader_ticker = normalize_ticker_(
        Config::get_or<std::string>("universe.affinity.leaderlag.require_leader", "BTC_USDT"));

    tick_controllers_(now, leader_ticker);

    // GAP-4: re-evaluate strategy affinity every 60s using fresh stats cache.
    // Handles funding spikes and spread widenings that disqualify tickers mid-session.
    if (now - last_affinity_refresh_ >= std::chrono::seconds(60)) {
        universe_.refresh_affinity();
        last_affinity_refresh_ = now;
    }

    strategy_engine_->tick(now);
    executor_->tick(now);

    bool events_occurred = process_closed_trades_(now);

    const auto& active = executor_->get_active_trades();

    account_state_.active_positions = 0;
    account_state_.active_tickers.clear();
    for (const auto& t : active) {
        if (t.state == TradeState::Open || t.state == TradeState::Exiting) {
            account_state_.active_positions++;
            account_state_.active_tickers.push_back(t.plan.ticker);
        }
    }
    account_state_.kill_switch_triggered = kill_switch_->is_triggered();

    static size_t last_active_count = 0;
    if (active.size() != last_active_count) {
        events_occurred = true;
        last_active_count = active.size();
    }

    // Inject mark prices; sum unrealized PnL in one pass (avoids re-reading order books).
    const auto all_marks = feed_->get_all_mark_prices();
    executor_->set_mark_prices(all_marks);
    double upnl_sum = 0.0;
    for (const auto& t : active) upnl_sum += t.unrealized_pnl;
    account_state_.unrealized_pnl_usd = upnl_sum;

    if (now - last_persist_ > std::chrono::seconds(10)) {
        std::vector<ActiveTrade> active_snapshot;
        for (const auto& t : active) {
            if (t.state != TradeState::Closed)
                active_snapshot.push_back(t);
        }
        persister_->save({account_state_, active_snapshot, last_reset_day_, false, ""});
        last_persist_ = now;
    }

    if (events_occurred || (now - last_dash_update_ > std::chrono::milliseconds(100))) {
        if (dashboard_->session_count() > 0) {
            const bool slow_tick = events_occurred ||
                (now - last_slow_dash_update_ > std::chrono::seconds(1));
            if (slow_tick) rebuild_slow_dash_cache_(now);
            send_dashboard_state_(now, active, upnl_sum, leader_ticker, all_marks);
        }
        // Only update last_dash_update_ when we actually sent — prevents slow_tick
        // suppression when the browser reconnects after a gap.
        last_dash_update_ = now;
    }
}

// ═══════════════════════════════════════════════════════════════
// Shutdown
// ═══════════════════════════════════════════════════════════════

void BotApp::handle_kill_switch_() {
    LOG_CRITICAL("Kill-switch triggered — running emergency shutdown sequence");

    // Step 1: Cancel all pending orders
    for (const auto& [ticker, _] : controllers_) {
        try {
            executor_->cancel_all(ticker);
            LOG_INFO("[KillSwitch] Cancelled orders for {}", ticker);
        } catch (const std::exception& e) {
            LOG_ERROR("[KillSwitch] Failed to cancel orders for {}: {}", ticker, e.what());
        }
    }

    // Step 2: Close all open positions
    for (const auto& trade : executor_->get_active_trades()) {
        if (trade.state == TradeState::Open && trade.executed_size > 0) {
            try {
                executor_->close_trade(trade.plan.ticker, FixedString<32>("KillSwitch"));
                LOG_INFO("[KillSwitch] Closed position for {}", trade.plan.ticker);
            } catch (const std::exception& e) {
                LOG_ERROR("[KillSwitch] Failed to close position for {}: {}", trade.plan.ticker, e.what());
            }
        }
    }

    // Step 3: Persist final state (best-effort — async fills will update on next run)
    if (persister_) {
        std::vector<ActiveTrade> active_snapshot;
        for (const auto& t : executor_->get_active_trades()) {
            if (t.state != TradeState::Closed)
                active_snapshot.push_back(t);
        }
        persister_->save({account_state_, active_snapshot, last_reset_day_, true, "KillSwitch"});
        LOG_INFO("[KillSwitch] Final state persisted");
    }

    // Step 4: Exit
    LOG_CRITICAL("Kill-switch shutdown complete — exiting with code 42");
    std::_Exit(42);
}

} // namespace trade_bot
