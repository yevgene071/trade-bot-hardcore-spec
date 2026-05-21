#include "app/BotApp.hpp"
#include "app/PipelineProcessor.hpp"

#include "control/KillSwitch.hpp"
#include "control/TickerController.hpp"
#include "executor/IExecutor.hpp"
#include "logger/Logger.hpp"
#include "logger/TradeJournal.hpp"
#include "marketdata/OrderBook.hpp"
#include "metrics/MetricsRegistry.hpp"
#include "perf/PerfRegistry.hpp"
#include "risk/AccountStatePersister.hpp"
#include "risk/RiskManager.hpp"
#include "risk/TradingDay.hpp"
#include "strategy/StrategyEngine.hpp"
#include "transport/FinresHandler.hpp"
#include "transport/MarketDataFeed.hpp"
#include "universe/TickerUniverse.hpp"

#include <spdlog/fmt/ranges.h>

#include <algorithm>
#include <fstream>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <map>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace trade_bot {

// ═══════════════════════════════════════════════════════════════
// Tick / Runtime
// ═══════════════════════════════════════════════════════════════

void BotApp::schedule_tick() {
    // 1Hz slow tick for non-hot-path operations (account sync, persistence, dashboard).
    // Hot path (OrderBook→Feature→Signal→Strategy→Risk→Submit) now runs event-driven
    // on processor_thread, triggered by each WS message without waiting for timer.
    timer_.expires_after(std::chrono::milliseconds(1000));
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

void BotApp::schedule_dashboard_tick() {
    // 50ms fast tick for real-time dashboard updates (20Hz)
    dashboard_timer_.expires_after(std::chrono::milliseconds(50));
    dashboard_timer_.async_wait([this](const boost::system::error_code& ec) {
        if (ec) return;
        if (!kill_switch_->is_triggered()) {
            dashboard_tick();
            schedule_dashboard_tick();
        } else {
            handle_kill_switch_();
        }
    });
}

void BotApp::dashboard_tick() {
    auto now = std::chrono::system_clock::now();
    
    if (dashboard_->session_count() == 0) return;
    
    // M4: Copy active trades before set_mark_prices to avoid reference invalidation
    // from concurrent processor thread modifications.
    const auto active = executor_->get_active_trades();

    // Inject mark prices; sum unrealized PnL
    const auto all_marks = feed_->get_all_mark_prices();
    executor_->set_mark_prices(all_marks);
    double upnl_sum = 0.0;
    for (const auto& t : active) upnl_sum += t.unrealized_pnl;
    
    const std::string leader_ticker = normalize_ticker_(
        Config::get_or<std::string>("universe.affinity.leaderlag.require_leader", "BTC_USDT"));
    
    // Rebuild slow cache every 100ms
    const bool slow_tick = (now - last_slow_dash_update_ > std::chrono::milliseconds(100));
    if (slow_tick) {
        rebuild_slow_dash_cache_(now);
        last_slow_dash_update_ = now;
    }
    
    send_dashboard_state_(now, active, upnl_sum, leader_ticker, all_marks, slow_tick);
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
        // M5: Re-seed accumulator from authoritative exchange balance to reset drift.
        free_balance_accumulator_ = KahanAccumulator<double>{};
        free_balance_accumulator_.add(finres->balance);
        account_state_.free_balance_usd = free_balance_accumulator_.sum();
        account_state_.equity_usd       = finres->equity;
    }
}

void BotApp::check_daily_reset_() {
    if (!TradingDay::is_new_day(last_reset_day_)) return;
    account_state_.starting_equity_usd    = account_state_.equity_usd;
    account_state_.realized_pnl_today_usd = 0.0;
    last_reset_day_ = TradingDay::current_date_utc();
    
    // P0 DETERMINISM: Reset Kahan accumulators on daily reset to prevent error accumulation across days
    realized_pnl_accumulator_ = KahanAccumulator<double>{};
    equity_accumulator_ = KahanAccumulator<double>{};
    equity_accumulator_.add(account_state_.equity_usd);
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
        if (funding) leader_frame->funding_rate = funding->rate;
        leader_frame->mark_price = feed_->get_mark_price(leader_ticker);
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
        if (funding) frame.funding_rate = funding->rate;
        frame.mark_price = feed_->get_mark_price(ticker);
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
    std::vector<IExecutor::ClosedTrade> closed;
    if (processor_) {
        processor_->drain_closed_trades(closed);
    } else {
        closed = executor_->pop_closed_trades();
    }
    for (const auto& ct : closed) {
        any_closed = true;
        // P0 DETERMINISM: Use Kahan accumulator for numerically stable PnL tracking
        realized_pnl_accumulator_.add(ct.pnl_usd);
        equity_accumulator_.add(ct.pnl_usd);
        account_state_.realized_pnl_today_usd = realized_pnl_accumulator_.sum();
        account_state_.equity_usd             = equity_accumulator_.sum();
        free_balance_accumulator_.add(ct.pnl_usd);
        account_state_.free_balance_usd        = free_balance_accumulator_.sum();
        risk_manager_->record_trade_end(ct.pnl_usd < 0, now);
        cached_consecutive_losses_ = (ct.pnl_usd < 0) ? cached_consecutive_losses_ + 1 : 0;
        TradeJournal::Entry je;
        je.plan           = ct.plan;
        je.frame_at_entry = ct.plan.frame_at_entry;
        je.pnl_usd        = ct.pnl_usd;
        je.exit_price     = ct.exit_price;
        je.cause_of_exit  = ct.reason;
        je.ts_unix_ms    = std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()).count();
        journal_.log_entry(je);
    }
    if (any_closed) journal_dirty_ = true;
    return any_closed;
}

void BotApp::rebuild_slow_dash_cache_(std::chrono::system_clock::time_point now) {
    LOG_TRACE("Dashboard update: journal={}, signals={}, universe={}, sessions={}",
             journal_.size(), recent_signals_.size(), universe_.active().size(),
             dashboard_->session_count());
    cached_journal_  = journal_.get_recent_entries(20);
    cached_signals_.assign(recent_signals_.begin(), recent_signals_.end());
    cached_sig_counts_ = signal_counts_;
    static auto last_1s_update = std::chrono::system_clock::time_point::min();
    bool slow_slow_tick = (now - last_1s_update >= std::chrono::seconds(1));
    
    if (slow_slow_tick) {
        last_1s_update = now;
        cached_universe_.clear();
        for (const auto& ticker : universe_.active()) {
            DashboardServer::State::UniverseRow row;
            row.ticker     = ticker;
            row.strategies = universe_.enabled_strategies(ticker);
            row.boosted    = universe_.is_boosted(ticker, now);
            cached_universe_.push_back(std::move(row));
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
    }

    cached_version_ = Config::get<std::string>("app.version");

    if (journal_dirty_ || cached_strategy_stats_.empty()) {
        cached_strategy_stats_.clear();
        {
            auto all_entries = journal_.get_recent_entries(200);
            absl::flat_hash_map<std::string, DashboardServer::State::StrategyStats> by_strat;
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
        journal_dirty_ = false;
    }

    // WS-02: cache expensive slow-changing fields for the fast tick path
    cached_strategy_states_ = strategy_engine_->get_all_states();

    std::string sel_ticker = dashboard_->get_selected_ticker();
    if (sel_ticker.empty()) {
        sel_ticker = normalize_ticker_(
            Config::get_or<std::string>("universe.affinity.leaderlag.require_leader", "BTC_USDT"));
    }
    if (auto it = controllers_.find(sel_ticker); it != controllers_.end()) {
        cached_chart_history_ = it->second->chart_snapshot();
        cached_density_history_ = it->second->density_snapshot();
        auto [bids, asks] = it->second->ob_snapshot(20);
        cached_bids_top20_ = std::move(bids);
        cached_asks_top20_ = std::move(asks);
        if (it->second->book) {
            auto best_bid = it->second->book->best_bid();
            auto best_ask = it->second->book->best_ask();
            if (best_bid && best_ask) {
                cached_ob_mid_        = 0.5 * (*best_bid + *best_ask);
                cached_ob_spread_bps_ = std::max(0.0, (*best_ask - *best_bid) / *best_bid * kBpsBase);
                double bid_d  = it->second->book->bid_depth(10);
                double ask_d  = it->second->book->ask_depth(10);
                double total  = bid_d + ask_d;
                cached_ob_imbalance_ = (total > 0) ? (bid_d - ask_d) / total : 0.0;
            }
        }
    }

    // Timestamp update moved to caller to fix 100ms timer bug
}

void BotApp::send_dashboard_state_(std::chrono::system_clock::time_point now,
                                   const std::vector<ActiveTrade>& active,
                                   double upnl_sum,
                                   const std::string& leader_ticker,
const absl::btree_map<Ticker, double>& all_marks,
                                   bool is_full_update) {
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
    dash_state.risk.total_trades_today   = static_cast<int>(journal_.size());
    dash_state.risk.consecutive_losses   = cached_consecutive_losses_;
    dash_state.risk.margin_used_pct      = eq > 0 ? ((eq - account_state_.free_balance_usd) / eq) * 100.0 : 0.0;
    dash_state.risk.exposure_pct         = (eq > 0 && !active.empty()) ? (upnl_sum / eq) * 100.0 : 0.0;
    dash_state.risk.daily_pnl_pct        = start_eq > 0 ? (account_state_.realized_pnl_today_usd / start_eq) * 100.0 : 0.0;
    dash_state.risk.current_drawdown_pct = start_eq > 0 ? ((eq - start_eq) / start_eq) * 100.0 : 0.0;
    dash_state.risk.max_positions        = static_cast<int>(Config::get_or<int64_t>("risk.max_concurrent_positions", 3));

    // WS-02: delta optimisation — is_full_update marks this as a slow tick
    // carrying heavy fields (strategy_states, chart, journal, etc). 
    dash_state.is_full_update = is_full_update;
    if (is_full_update) {
        dash_state.strategy_states = cached_strategy_states_;
        dash_state.chart_history   = cached_chart_history_;
        dash_state.density_history = cached_density_history_;
    }
    
    // Real-time fields: always include the latest OB and price data for the selected ticker
    // to ensure the Ladder and charts feel responsive.
    std::string sel_ticker = dashboard_->get_selected_ticker();
    if (sel_ticker.empty()) sel_ticker = leader_ticker;
    dash_state.selected_ticker = sel_ticker;

    if (auto it = controllers_.find(sel_ticker);
        it != controllers_.end() && it->second->book) {
        const auto bb = it->second->book->best_bid();
        const auto ba = it->second->book->best_ask();
        if (bb && ba) {
            dash_state.ob_mid        = 0.5 * (*bb + *ba);
            dash_state.ob_spread_bps = std::max(0.0, (*ba - *bb) / *bb * kBpsBase);
            const double bd = it->second->book->bid_depth(10);
            const double ad = it->second->book->ask_depth(10);
            const double tot = bd + ad;
            dash_state.ob_imbalance  = (tot > 0) ? (bd - ad) / tot : 0.0;
        }
        // Always include top 20 levels for smooth Ladder updates
        auto [bids, asks] = it->second->ob_snapshot(20);
        dash_state.bids_top20 = std::move(bids);
        dash_state.asks_top20 = std::move(asks);
    } else {
        dash_state.ob_mid          = cached_ob_mid_;
        dash_state.ob_spread_bps   = cached_ob_spread_bps_;
        dash_state.ob_imbalance    = cached_ob_imbalance_;
        dash_state.bids_top20      = cached_bids_top20_;
        dash_state.asks_top20      = cached_asks_top20_;
    }

    dashboard_->update_state_async(std::move(dash_state));
}

void BotApp::tick() {
    auto now = std::chrono::system_clock::now();

    // Slow path (1Hz): account sync, persistence, sanity checks, dashboard
    update_account_from_exchange_();
    check_daily_reset_();

    const std::string leader_ticker = normalize_ticker_(
        Config::get_or<std::string>("universe.affinity.leaderlag.require_leader", "BTC_USDT"));

    // NOTE: tick_controllers_() removed — feature extraction now happens event-driven
    // on processor_thread, triggered by each WS message (OrderBook/Trade update).
    // Controllers still exist for slow-path operations (chart snapshots, OB snapshots).

    // Sanity check for OrderBooks: periodic resync via fresh snapshots to detect missed WS gaps.
    if (now - last_ob_sanity_check_ >= std::chrono::seconds(30)) {
        for (const auto& [ticker, _] : controllers_) {
            feed_->force_resync_orderbook(ticker);
        }
        last_ob_sanity_check_ = now;
    }

    // GAP-4: re-evaluate strategy affinity every 60s using fresh stats cache.
    // Handles funding spikes and spread widenings that disqualify tickers mid-session.
    if (now - last_affinity_refresh_ >= std::chrono::seconds(60)) {
        universe_.refresh_affinity();
        last_affinity_refresh_ = now;
    }

    // Strategy tick for TTL-based logic (timeouts, waiting periods)
    strategy_engine_->tick(now);
    
    // Executor tick for TP/SL TTL checks (1Hz is sufficient)
    executor_->tick(now);

    // Process closed trades from MPSC queue (populated by processor thread)
    process_closed_trades_(now);

    // M6: Copy active trades snapshot — processor thread may modify executor state concurrently.
    const auto active = executor_->get_active_trades();

    account_state_.active_positions = 0;
    account_state_.active_tickers.clear();
    for (const auto& t : active) {
        if (t.state == TradeState::Open || t.state == TradeState::Exiting) {
            account_state_.active_positions++;
            account_state_.active_tickers.push_back(t.plan.ticker);
        }
    }
    account_state_.kill_switch_triggered = kill_switch_->is_triggered();

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

    // Dashboard updates now handled by separate 50ms timer (dashboard_tick)
}

// ═══════════════════════════════════════════════════════════════
// Shutdown
// ═══════════════════════════════════════════════════════════════

void BotApp::handle_kill_switch_() {
    // Guard: only the first caller runs the sequence; the other (from dashboard vs slow tick) returns.
    if (shutdown_initiated_.exchange(true)) return;

    LOG_CRITICAL("Kill-switch triggered — running emergency shutdown sequence");

    const auto seq_start = std::chrono::steady_clock::now();
    const auto total_deadline = seq_start + std::chrono::seconds(
        Config::get_or<int64_t>("risk.killswitch_total_timeout_sec", 45));

    auto deadline_ok = [&]() {
        return std::chrono::steady_clock::now() < total_deadline;
    };

    // Step 0: Dump performance report
    dump_perf_report_();

    // Step 2: Cancel all open limit orders
    LOG_INFO("[KillSwitch] Step 2: cancelling all orders");
    for (const auto& [ticker, _] : controllers_) {
        if (!deadline_ok()) { LOG_ERROR("[KillSwitch] Total deadline exceeded at Step 2"); break; }
        try {
            executor_->cancel_all(ticker);
            LOG_INFO("[KillSwitch] Cancelled orders for {}", ticker);
        } catch (const std::exception& e) {
            LOG_ERROR("[KillSwitch] cancel_all {}: {}", ticker, e.what());
        }
    }

    // Step 3: Get actual positions from exchange via REST (don't trust internal model)
    LOG_INFO("[KillSwitch] Step 3: fetching positions from exchange");
    std::vector<PositionUpdate> rest_positions;
    if (gateway_ && live_executor_ && deadline_ok()) {
        try {
            rest_positions = gateway_->get_positions(m_connection_id_for_tick);
            LOG_INFO("[KillSwitch] Got {} REST positions", rest_positions.size());
        } catch (const std::exception& e) {
            LOG_ERROR("[KillSwitch] get_positions failed: {} — falling back to internal model", e.what());
            for (const auto& t : executor_->get_active_trades()) {
                if (t.state == TradeState::Open && t.executed_size > 0) {
                    PositionUpdate p;
                    p.ticker = t.plan.ticker;
                    p.size   = t.executed_size;
                    rest_positions.push_back(p);
                }
            }
        }
    } else {
        for (const auto& t : executor_->get_active_trades()) {
            if (t.state == TradeState::Open && t.executed_size > 0) {
                PositionUpdate p;
                p.ticker = t.plan.ticker;
                p.size   = t.executed_size;
                rest_positions.push_back(p);
            }
        }
    }

    // Step 4: Close all open positions via market order
    LOG_INFO("[KillSwitch] Step 4: closing {} position(s)", rest_positions.size());
    for (const auto& pos : rest_positions) {
        if (std::abs(pos.size) < 1e-9) continue;
        if (!deadline_ok()) { LOG_ERROR("[KillSwitch] Total deadline exceeded at Step 4"); break; }
        try {
            executor_->close_trade(pos.ticker, FixedString<32>("KillSwitch"));
            LOG_INFO("[KillSwitch] Market-close sent for {}", pos.ticker);
        } catch (const std::exception& e) {
            LOG_ERROR("[KillSwitch] close_trade {}: {}", pos.ticker, e.what());
        }
    }

    // Step 5: Poll until all positions closed (500ms interval, 30s timeout, 3 retries per position)
    if (gateway_ && live_executor_ && deadline_ok()) {
        LOG_INFO("[KillSwitch] Step 5: polling until positions closed");
        const auto close_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(
            Config::get_or<int64_t>("risk.kill_switch_close_timeout_sec", 30));
        std::map<Ticker, int> retries;

        while (deadline_ok() && std::chrono::steady_clock::now() < close_deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            try {
                auto positions = gateway_->get_positions(m_connection_id_for_tick);
                bool all_closed = true;
                for (const auto& p : positions) {
                    if (std::abs(p.size) < 1e-9) continue;
                    all_closed = false;
                    int& r = retries[p.ticker];
                    if (r < 3) {
                        LOG_WARN("[KillSwitch] {} open (size={}), retry {}", p.ticker, p.size, r + 1);
                        try { executor_->close_trade(p.ticker, FixedString<32>("KillSwitch")); } catch (...) {}
                        ++r;
                    } else {
                        LOG_ERROR("[KillSwitch] {} not closed after 3 retries", p.ticker);
                    }
                }
                if (all_closed) { LOG_INFO("[KillSwitch] All positions confirmed closed"); break; }
            } catch (const std::exception& e) {
                LOG_ERROR("[KillSwitch] poll positions: {}", e.what());
            }
        }
    }

    // Step 6: killswitch file already written + fsync'd by KillSwitch::trigger()

    // Step 7: Persist final state + kill_switch_report.json
    LOG_INFO("[KillSwitch] Step 7: persisting final state");
    if (persister_) {
        std::vector<ActiveTrade> active_snapshot;
        for (const auto& t : executor_->get_active_trades()) {
            if (t.state != TradeState::Closed)
                active_snapshot.push_back(t);
        }
        persister_->save({account_state_, active_snapshot, last_reset_day_, true, "KillSwitch"});

        auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - seq_start).count();
        std::ofstream report("journal/kill_switch_report.json");
        if (report) {
            report << "{\n"
                   << "  \"kill_switch_triggered\": true,\n"
                   << "  \"elapsed_ms\": " << elapsed_ms << ",\n"
                   << "  \"incomplete\": " << (!deadline_ok() ? "true" : "false") << ",\n"
                   << "  \"positions_at_start\": " << rest_positions.size() << "\n"
                   << "}\n";
        }
        LOG_INFO("[KillSwitch] Final state persisted ({}ms)", elapsed_ms);
    }

    // Step 8: Exit
    LOG_CRITICAL("Kill-switch shutdown complete — exiting with code 42");
    spdlog::shutdown();
    std::exit(42);
}

void BotApp::dump_perf_report_() {
    if (!Logger::get()) return;

    auto report = PerfRegistry::instance().render_text_report();

    auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    std::string filename = "logs/perf-" + std::to_string(now_ms) + ".txt";

    std::ofstream ofs(filename);
    if (ofs) {
        ofs << report;
        LOG_INFO("Performance report written to {}", filename);
    } else {
        LOG_ERROR("Failed to write performance report to {}", filename);
    }

    LOG_CRITICAL("=== Performance Report ===\n{}", report);
}

} // namespace trade_bot

