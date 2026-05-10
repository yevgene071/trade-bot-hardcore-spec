#include "logger/Logger.hpp"
#include "logger/TradeJournal.hpp"
#include "config/Config.hpp"
#include "control/KillSwitch.hpp"
#include "control/ClockDriftMonitor.hpp"
#include "control/SntpClient.hpp"
#include "control/TickerController.hpp"
#include "transport/MarketDataFeed.hpp"
#include "transport/NotificationFeed.hpp"
#include "transport/DumpRecorder.hpp"
#include "transport/MetaScalpCodec.hpp"
#include "transport/MetaScalpDiscovery.hpp"
#include "transport/OrderGateway.hpp"
#include "transport/BeastWsClient.hpp"
#include "transport/CurlHttpClient.hpp"
#include "transport/FinresHandler.hpp"
#include "transport/ClusterSnapshotClient.hpp"
#include "transport/external/ExternalIoContext.hpp"
#include "transport/external/FeedStalenessMonitor.hpp"
#include "marketdata/ClusterSnapshot.hpp"
#include "universe/TickerUniverse.hpp"
#include "universe/UniverseFilters.hpp"
#include "strategy/StrategyEngine.hpp"
#include "strategy/BounceFromDensity.hpp"
#include "strategy/BreakoutEatThrough.hpp"
#include "strategy/LeaderLag.hpp"
#include "executor/PaperExecutor.hpp"
#include "risk/RiskManager.hpp"
#include "risk/AccountState.hpp"
#include "risk/AccountStatePersister.hpp"
#include "risk/TradingDay.hpp"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <spdlog/fmt/ranges.h>
#include <iostream>
#include <memory>
#include <thread>
#include <deque>
#include <map>
#include <boost/asio/io_context.hpp>
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/steady_timer.hpp>

#include "control/DashboardServer.hpp"
#include "metrics/MetricsExporter.hpp"
#include "metrics/MetricsRegistry.hpp"
#include "metrics/AlertWebhook.hpp"

using namespace trade_bot;

class BotApp {
public:
    explicit BotApp(boost::asio::io_context& ioc)
        : ioc_(ioc)
        , timer_(ioc)
        , last_reset_day_(TradingDay::current_date_utc())
        , last_persist_(std::chrono::system_clock::now())
        , last_dash_update_(std::chrono::system_clock::now())
        , last_slow_dash_update_(std::chrono::system_clock::time_point{})  // force first slow tick
        , kill_switch_(&KillSwitch::instance()) {}
    void run() {
        init_components();
        schedule_tick();
    }

private:
    void init_components() {
        Logger::init();
        persister_ = std::make_unique<AccountStatePersister>("journal/account_state.json");

        if (std::filesystem::exists("./killswitch")) {
            throw std::runtime_error("Kill-switch file present");
        }

        if (!std::filesystem::exists("config.toml")) {
            throw std::runtime_error("Config 'config.toml' not found");
        }
        Config::load("config.toml");

        LOG_INFO("trade_bot {} starting...", Config::get<std::string>("app.version"));

        kill_switch_ = &KillSwitch::instance();
        kill_switch_->start();

        auto ntp = std::make_shared<SntpClient>();
        clock_monitor_ = std::make_unique<ClockDriftMonitor>(ntp, ClockDriftMonitor::Config{
            .warn_drift_ms      = Config::get_or<int64_t>("clock.warn_drift_ms",      500),
            .max_clock_drift_ms = Config::get_or<int64_t>("clock.max_clock_drift_ms", 2000),
        });
        clock_monitor_->start();

        // ExternalIoContext still used by ClusterSnapshot and AlertWebhook.
        ExternalIoContext::instance().start();

        const auto dashboard_addr = Config::get_or<std::string>("dashboard.bind_address", "127.0.0.1");
        const auto dashboard_port = static_cast<uint16_t>(Config::get_or<int64_t>("dashboard.port", 8080));
        const auto dashboard_token = Config::get_or<std::string>("dashboard.auth_token", std::string{});
        dashboard_ = std::make_unique<DashboardServer>(dashboard_ioc_, dashboard_addr, dashboard_port, dashboard_token);
        dashboard_->start();

        const auto metrics_port = static_cast<uint16_t>(Config::get_or<int64_t>("metrics.port", 9090));
        const auto metrics_addr = Config::get_or<std::string>("metrics.bind_address", "127.0.0.1");
        const auto metrics_token = Config::get_or<std::string>("metrics.auth_token", std::string{});
        metrics_exporter_ = std::make_unique<MetricsExporter>(dashboard_ioc_, metrics_addr, metrics_port, metrics_token);
        metrics_exporter_->start();

        dashboard_thread_ = std::thread([this]() {
            auto work = boost::asio::make_work_guard(dashboard_ioc_);
            dashboard_ioc_.run();
        });

        signal_bus_ = std::make_unique<SignalBus>();
        signal_bus_->subscribe([&](const Signal& s) {
            MetricsRegistry::instance().counter_inc("trade_bot_signals_total", {{"kind", std::to_string(static_cast<int>(s.kind))}});
            static constexpr const char* kKindNames[] = {
                "DensityDetected","DensityRemoved","DensityEating",
                "IcebergSuspected","TapeBurst","TapeFade","TapeFlush","TapeDistribution",
                "LevelFormed","LevelApproach","LevelRejection","LevelBreak","LeaderMove"};
            auto idx = static_cast<std::size_t>(s.kind);
            const char* kind_name = idx < std::size(kKindNames) ? kKindNames[idx] : "Unknown";
            signal_counts_[kind_name]++;

            // Ring buffer of recent signal events for the dashboard feed.
            // Called on ioc_ thread (same as tick()), so no mutex needed.
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                s.timestamp.time_since_epoch()) % 1000;
            auto tt = std::chrono::system_clock::to_time_t(s.timestamp);
            std::tm tm_buf{};
            gmtime_r(&tt, &tm_buf);
            char tbuf[16];
            std::snprintf(tbuf, sizeof(tbuf), "%02d:%02d:%02d.%03d",
                          tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec,
                          static_cast<int>(ms.count()));
            DashboardServer::State::SignalEvent ev;
            ev.kind       = kind_name;
            ev.ticker     = s.ticker;
            ev.price      = s.price;
            ev.confidence = s.confidence;
            ev.time_str   = tbuf;
            recent_signals_.push_front(std::move(ev));
            if (recent_signals_.size() > 60) recent_signals_.pop_back();
        });

        strategy_engine_ = std::make_unique<StrategyEngine>(*signal_bus_);
        risk_manager_ = std::make_unique<RiskManager>(universe_, news_);
        
        auto persisted = persister_->load();
        if (persisted) {
            account_state_ = persisted->account_state;
            last_reset_day_ = persisted->last_reset_day_utc;
        } else {
            account_state_.equity_usd          = 10000.0;
            account_state_.starting_equity_usd = 10000.0;
            last_reset_day_ = TradingDay::current_date_utc();
        }
        account_state_.trading_day_start = std::chrono::system_clock::now();

        // T4-RISK: Load news calendar for blackout protection
        if (std::filesystem::exists("config/news.json")) {
            try {
                news_.load("config/news.json");
                news_.start_watching();
            } catch (const std::exception& e) {
                LOG_WARN("Failed to load news calendar: {}", e.what());
            }
        }

        auto http = std::make_shared<CurlHttpClient>();
        auto ws = std::make_shared<BeastWsClient>(ioc_);
        
        MetaScalpDiscovery discovery(http);
        auto port = discovery.discover();
        if (!port) throw std::runtime_error("MetaScalp not found");

        auto gateway = std::make_shared<OrderGateway>(http);
        gateway->set_port(*port);
        
        int connection_id = 1; // Default
        try {
            auto connections = gateway->get_connections();
            for (const auto& conn : connections) {
                if (conn.name.find("Futures") != std::string::npos) {
                    connection_id = conn.id;
                    LOG_INFO("Selected connection {}: {}", connection_id, conn.name);
                    break;
                }
            }
        } catch (const std::exception& e) {
            LOG_WARN("Failed to get connections, using default ID 1: {}", e.what());
        }

        feed_ = std::make_unique<MarketDataFeed>(ws, connection_id);
        feed_->set_record_tap([this](const nlohmann::json& msg, int64_t ts_ns) {
            dump_recorder_.record(msg, ts_ns);
        });
        dashboard_->set_recorder(&dump_recorder_);
        cluster_client_ = std::make_unique<ClusterSnapshotClient>(*http, "http://localhost:" + std::to_string(*port), connection_id);
        cluster_mgr_ = std::make_unique<ClusterSnapshotManager>(*cluster_client_);
        cluster_mgr_->start();

        // T4-EXECUTOR: Respect mode config (paper vs live)
        const std::string mode = Config::get_or<std::string>("executor.mode", "paper");
        if (mode == "live") {
            executor_ = std::make_unique<LiveExecutor>(connection_id, *gateway, *feed_);
        } else {
            executor_ = std::make_unique<PaperExecutor>(books_for_executor_);
        }
        feed_->add_listener(executor_.get());

        // T4-RECOVERY: Run startup recovery and inject trades
        StartupRecovery recovery(connection_id, *gateway, *persister_);
        auto recovery_res = recovery.run();
        for (const auto& entry : recovery_res.log_entries) LOG_INFO("[Recovery] {}", entry);
        executor_->inject_recovered_trades(recovery_res.recovered_trades);

        strategy_engine_->set_on_plan([&](const TradePlan& plan) {
            auto decision = risk_manager_->evaluate(plan, account_state_);
            if (decision.accepted) {
                TradePlan accepted_plan = plan;
                accepted_plan.size_coin = decision.adjusted_size_coin;
                
                // T4-RISK: Normalize prices for exchange compatibility
                auto meta = universe_.meta(plan.ticker).value_or(TickerMeta{0.01, 1e-6, 0.0, 0.0});
                if (meta.price_increment > 0.0) {
                    auto round_p = [&](double p) {
                        return std::round(p / meta.price_increment) * meta.price_increment;
                    };
                    accepted_plan.entry_price = round_p(accepted_plan.entry_price);
                    accepted_plan.stop_price  = round_p(accepted_plan.stop_price);
                    accepted_plan.tp1_price   = round_p(accepted_plan.tp1_price);
                    if (accepted_plan.tp2_price) {
                        accepted_plan.tp2_price = round_p(*accepted_plan.tp2_price);
                    }
                }

                executor_->submit(accepted_plan);
            }
        });

        universe_.set_affinity_change_handler([&](const Ticker& ticker, const std::string& strategy, bool enabled) {
            LOG_INFO("[Universe] {} {} for {}", enabled ? "ENABLED" : "DISABLED", strategy, ticker);
            if (enabled) {
                if (!controllers_.contains(ticker)) {
                    // T4-LEADER: Pass configured leader ticker to controller (#162)
                    std::string leader_name = Config::get_or<std::string>("universe.affinity.leaderlag.require_leader", "BTC_USDT");
                    // Normalize (BTCUSDT -> BTC_USDT)
                    if (leader_name.find('_') == std::string::npos && leader_name.size() > 4) {
                        leader_name = leader_name.substr(0, leader_name.size() - 4) + "_" + leader_name.substr(leader_name.size() - 4);
                    }
                    
                    controllers_[ticker] = std::make_unique<TickerController>(ticker, *signal_bus_, universe_, *cluster_mgr_, leader_name);
                    books_for_executor_[ticker] = controllers_[ticker]->book.get();
                    feed_->add_listener(ticker, controllers_[ticker].get());
                    feed_->subscribe_ticker(ticker);  // also sends funding_subscribe + mark_price_subscribe

                    // Ensure leader is also subscribed if it's not the current ticker
                    if (leader_name != ticker && !controllers_.contains(leader_name)) {
                        controllers_[leader_name] = std::make_unique<TickerController>(leader_name, *signal_bus_, universe_, *cluster_mgr_);
                        books_for_executor_[leader_name] = controllers_[leader_name]->book.get();
                        feed_->add_listener(leader_name, controllers_[leader_name].get());
                        feed_->subscribe_ticker(leader_name);
                    }
                }
                if (strategy == "bounce") strategy_engine_->add_strategy(std::make_unique<BounceFromDensity>(ticker));
                else if (strategy == "breakout") strategy_engine_->add_strategy(std::make_unique<BreakoutEatThrough>(ticker));
                else if (strategy == "leaderlag") strategy_engine_->add_strategy(std::make_unique<LeaderLag>(ticker));
            } else {
                std::string cls;
                if (strategy == "bounce") cls = "BounceFromDensity";
                else if (strategy == "breakout") cls = "BreakoutEatThrough";
                else if (strategy == "leaderlag") cls = "LeaderLag";
                if (!cls.empty()) strategy_engine_->remove_strategy(ticker, cls);
                
                // T4-LIFECYCLE: Cleanup controller if no strategies left for this ticker
                // and no active positions in PaperExecutor.
                if (universe_.enabled_strategies(ticker).empty()) {
                    bool has_position = false;
                    for (const auto& t : executor_->get_active_trades()) {
                        if (t.plan.ticker == ticker && t.state != TradeState::Closed) {
                            has_position = true;
                            break;
                        }
                    }
                    if (!has_position) {
                        LOG_INFO("[Universe] Removing controller for {} (idle)", ticker);
                        feed_->remove_listener(ticker, controllers_[ticker].get());
                        feed_->unsubscribe_ticker(ticker);
                        books_for_executor_.erase(ticker);
                        controllers_.erase(ticker);
                    }
                }
            }
        });

        universe_.register_strategy("bounce", [](const Ticker&){ return true; });
        universe_.register_strategy("breakout", [](const Ticker&){ return true; });
        universe_.register_strategy("leaderlag", [](const Ticker&){ return true; });

        ws->connect("ws://127.0.0.1:" + std::to_string(*port) + "/ws");
        feed_->start();

        // -- Universe discovery -------------------------------------------------
        auto allow_pats = Config::get_or<std::vector<std::string>>(
            "universe.pool.allow_patterns", std::vector<std::string>{"*USDT"});
        auto deny_pats  = Config::get_or<std::vector<std::string>>(
            "universe.pool.deny_patterns",
            std::vector<std::string>{"*UP*", "*DOWN*", "*BULL*", "*BEAR*", "*1000*"});
        auto operator_allow = Config::get_or<std::vector<std::string>>(
            "universe.pool.manual_allow",
            Config::get_or<std::vector<std::string>>("trading.symbols",
                std::vector<std::string>{}));

        // Fetch all tickers, apply static filter, cache meta for every candidate.
        std::vector<Ticker> all_candidates;
        try {
            auto all_infos = gateway->get_tickers(connection_id);
            LOG_INFO("[Universe] Fetched {} tickers from MetaScalp", all_infos.size());

            UniverseFilters::Config flt_cfg;
            flt_cfg.allow_patterns = allow_pats;
            flt_cfg.deny_patterns  = deny_pats;
            UniverseFilters static_flt(flt_cfg);

            for (auto& info : all_infos) {
                if (!info.is_trading_allowed)       continue;
                if (!static_flt.accepts(info.name)) continue;
                universe_.cache_meta(info.name, TickerMeta{
                    info.price_increment, info.size_increment,
                    info.min_size, info.max_size});
                all_candidates.push_back(info.name);
            }
            LOG_INFO("[Universe] {}/{} tickers pass static filter",
                     all_candidates.size(), all_infos.size());
        } catch (const std::exception& e) {
            LOG_WARN("[Universe] get_tickers failed ({}), using operator list only", e.what());
            all_candidates = operator_allow;
        }

        TickerUniverse::Config u_cfg;
        u_cfg.filters.allow_patterns = allow_pats;
        u_cfg.filters.deny_patterns  = deny_pats;
        u_cfg.filters.manual_allow   = operator_allow;
        u_cfg.max_pool_size = static_cast<std::size_t>(
            Config::get_or<int64_t>("universe.pool.max_pool_size", 30));
        universe_.update_config(u_cfg);

        // Seed full candidate list so on_screener_new_coin() can build the pool.
        universe_.seed_candidates(all_candidates);

        // Initial pool: operator's manual_allow list (bypasses stats gate).
        // The screener (NotificationFeed) will expand this dynamically.
        universe_.refresh_pool(operator_allow);
        universe_.refresh_affinity();
        LOG_INFO("[Universe] Initial pool ({} tickers): [{}]", universe_.active().size(),
                 fmt::join(universe_.active(), ", "));

        // -- Notification feed (screener + BigTick) ----------------------------
        // Uses a separate WS connection so its on_message handler does not
        // conflict with MarketDataFeed's handler on the same socket.
        auto notif_ws = std::make_shared<BeastWsClient>(ioc_);
        notif_feed_ = std::make_unique<NotificationFeed>(notif_ws, universe_,
                                                         NotificationFeed::Config{});
        notif_ws->connect("ws://127.0.0.1:" + std::to_string(*port) + "/ws");
        notif_feed_->start();
        LOG_INFO("[Universe] NotificationFeed started — screener will populate pool");
    }

    void schedule_tick() {
        timer_.expires_after(std::chrono::milliseconds(100));
        timer_.async_wait([this](const boost::system::error_code& ec) {
            if (!ec && !kill_switch_->is_triggered()) {
                tick();
                schedule_tick();
            }
        });
    }

    void tick() {
        auto now = std::chrono::system_clock::now();
        
        if (TradingDay::is_new_day(last_reset_day_)) {
            account_state_.starting_equity_usd = account_state_.equity_usd;
            account_state_.realized_pnl_today_usd = 0.0;
            last_reset_day_ = TradingDay::current_date_utc();
        }

        // T4-LEADER: Multi-pass tick for leader-follower coordination (#162)
        std::string leader_ticker = Config::get_or<std::string>("universe.affinity.leaderlag.require_leader", "BTC_USDT");
        if (leader_ticker.find('_') == std::string::npos && leader_ticker.size() > 4) {
            leader_ticker = leader_ticker.substr(0, leader_ticker.size() - 4) + "_" + leader_ticker.substr(leader_ticker.size() - 4);
        }

        std::optional<FeatureFrame> leader_frame;
        if (auto it = controllers_.find(leader_ticker); it != controllers_.end()) {
            auto funding = feed_->get_funding(leader_ticker);
            if (funding && last_funding_times_[leader_ticker] != funding->next_funding_time) {
                risk_manager_->update_funding_time(leader_ticker, funding->next_funding_time);
                last_funding_times_[leader_ticker] = funding->next_funding_time;
            }
            leader_frame = it->second->tick(now);
            strategy_engine_->on_frame(*leader_frame);
        }

        for (auto& [ticker, ctrl] : controllers_) {
            if (ticker == leader_ticker) continue; // already processed

            auto funding = feed_->get_funding(ticker);
            if (funding && last_funding_times_[ticker] != funding->next_funding_time) {
                risk_manager_->update_funding_time(ticker, funding->next_funding_time);
                last_funding_times_[ticker] = funding->next_funding_time;
            }
            
            if (leader_frame) {
                ctrl->on_leader_frame(*leader_frame);
            }

            auto frame = ctrl->tick(now);
            strategy_engine_->on_frame(frame);
        }
        
        strategy_engine_->tick(now);
        executor_->tick(now);

        // Apply closed trades → update equity and write journal
        bool events_occurred = false;
        for (const auto& ct : executor_->pop_closed_trades()) {
            events_occurred = true;
            account_state_.realized_pnl_today_usd += ct.pnl_usd;
            account_state_.equity_usd             += ct.pnl_usd;
            account_state_.free_balance_usd       += ct.pnl_usd;
            
            // T4-RISK: Notify risk manager of trade completion for streak tracking
            risk_manager_->record_trade_end(ct.pnl_usd < 0, now);

            TradeJournal::Entry je;
            je.plan          = ct.plan;
            je.pnl_usd       = ct.pnl_usd;
            je.exit_price    = ct.exit_price;
            je.cause_of_exit = ct.reason;
            journal_.log_entry(je);
        }
        
        const auto& active = executor_->get_active_trades();
        
        // T4-RISK: Synchronize AccountState with current execution state
        account_state_.active_positions = 0;
        account_state_.active_tickers.clear();
        for (const auto& t : active) {
            if (t.state == TradeState::Open || t.state == TradeState::Exiting) {
                account_state_.active_positions++;
                account_state_.active_tickers.push_back(t.plan.ticker);
            }
        }
        account_state_.kill_switch_triggered = kill_switch_->is_triggered();

        // Trigger dashboard update if position count changed (trade opened/closed)
        static size_t last_active_count = 0;
        if (active.size() != last_active_count) {
            events_occurred = true;
            last_active_count = active.size();
        }

        // Unrealized PnL is already computed inside get_active_trades() for each
        // position; sum it here to avoid a redundant second pass through the
        // order books (was: executor_->unrealized_pnl() which re-reads bid/ask).
        // Inject mark prices so PnL uses exchange mark (consistent with dashboard).
        const auto all_marks = feed_->get_all_mark_prices();  // one lock per tick
        executor_->set_mark_prices(all_marks);
        double upnl_sum = 0.0;
        for (const auto& t : active) upnl_sum += t.unrealized_pnl;
        account_state_.unrealized_pnl_usd = upnl_sum;

        if (now - last_persist_ > std::chrono::seconds(10)) {
            persister_->save({account_state_, {}, last_reset_day_, false, ""});
            last_persist_ = now;
        }

        if (events_occurred || (now - last_dash_update_ > std::chrono::milliseconds(100))) {
            // T4-PERF: Skip expensive state building if no one is watching (#161)
            if (dashboard_->session_count() > 0) {
                // Slow-changing data: journal, signals, universe strategies/boosted.
                // Rebuilding these every 100ms is wasteful — cache and refresh every 1s
                // or when a trade event happens (journal changes).
                const bool slow_tick = events_occurred ||
                    (now - last_slow_dash_update_ > std::chrono::seconds(1));
                if (slow_tick) {
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
                    last_slow_dash_update_ = now;
                }
                // Fast path: only update prices, equity, positions (cheap)
                DashboardServer::State dash_state;
                dash_state.account = account_state_;
                dash_state.open_trades = active;
                dash_state.kill_switch_active = kill_switch_->is_triggered();
                dash_state.version = cached_version_;
                dash_state.signal_counts = cached_sig_counts_;
                dash_state.recent_journal = cached_journal_;
                dash_state.recent_signals = cached_signals_;
                // Reuse all_marks from above — no second lock
                dash_state.universe_rows = cached_universe_;
                for (auto& row : dash_state.universe_rows) {
                    auto mit = all_marks.find(row.ticker);
                    if (mit != all_marks.end()) row.mark_price = mit->second;
                }
                dashboard_->update_state(dash_state);
            }
            last_dash_update_ = now;
        }
    }

private:
    boost::asio::io_context& ioc_;
    // Separate io_context for HTTP (dashboard + metrics) so market data WS
    // processing on ioc_ cannot starve HTTP responses.
    boost::asio::io_context dashboard_ioc_;
    std::thread dashboard_thread_;
    boost::asio::steady_timer timer_;

    TickerUniverse universe_;
    NewsCalendar news_;
    AccountState account_state_;
    std::string last_reset_day_;
    std::chrono::system_clock::time_point last_persist_;
    std::chrono::system_clock::time_point last_dash_update_;
    std::chrono::system_clock::time_point last_slow_dash_update_;

    // Cached slow-changing dashboard data (rebuilt every 1s, not every 100ms)
    std::vector<TradeJournal::Entry> cached_journal_;
    std::vector<DashboardServer::State::SignalEvent> cached_signals_;
    std::vector<DashboardServer::State::UniverseRow> cached_universe_;
    std::map<std::string, int> cached_sig_counts_;
    std::string cached_version_;

    DumpRecorder dump_recorder_;
    KillSwitch* kill_switch_;
    std::unique_ptr<ClockDriftMonitor> clock_monitor_;
    std::unique_ptr<DashboardServer> dashboard_;
    std::unique_ptr<MetricsExporter> metrics_exporter_;
    std::unique_ptr<SignalBus> signal_bus_;
    std::unique_ptr<StrategyEngine> strategy_engine_;
    std::unique_ptr<RiskManager> risk_manager_;
    std::unique_ptr<MarketDataFeed> feed_;
    std::unique_ptr<NotificationFeed> notif_feed_;
    std::unique_ptr<ClusterSnapshotClient> cluster_client_;
    std::unique_ptr<ClusterSnapshotManager> cluster_mgr_;
    std::unique_ptr<IExecutor> executor_;
    std::unique_ptr<AccountStatePersister> persister_;

    std::map<Ticker, std::unique_ptr<TickerController>> controllers_;
    std::map<Ticker, const OrderBook*> books_for_executor_;
    std::map<Ticker, std::chrono::system_clock::time_point> last_funding_times_;
    std::map<std::string, int> signal_counts_;
    std::deque<DashboardServer::State::SignalEvent> recent_signals_; // newest first, max 60
    TradeJournal journal_{"journal"};
};

int main() {
    try {
        boost::asio::io_context ioc;
        BotApp app(ioc);
        app.run();
        ioc.run();
    } catch (const std::exception& e) {
        std::cerr << "Fatal error: " << e.what() << std::endl;
        return 1;
    }
    return 0;
}

