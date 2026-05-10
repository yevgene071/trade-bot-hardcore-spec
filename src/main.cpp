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
        // Guard against persisted state saved before starting_equity was set.
        if (account_state_.starting_equity_usd <= 0.0)
            account_state_.starting_equity_usd = account_state_.equity_usd;
        // In paper mode there are no exchange balance updates, so seed free_balance
        // from equity so the dashboard shows a non-zero figure.
        if (account_state_.free_balance_usd == 0.0 && account_state_.equity_usd > 0.0)
            account_state_.free_balance_usd = account_state_.equity_usd;

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

        executor_ = std::make_unique<PaperExecutor>(books_for_executor_);

        strategy_engine_->set_on_plan([&](const TradePlan& plan) {
            auto decision = risk_manager_->evaluate(plan, account_state_);
            if (decision.accepted) {
                TradePlan accepted_plan = plan;
                accepted_plan.size_coin = decision.adjusted_size_coin;
                executor_->submit(accepted_plan);
            }
        });

        universe_.set_affinity_change_handler([&](const Ticker& ticker, const std::string& strategy, bool enabled) {
            LOG_INFO("[Universe] {} {} for {}", enabled ? "ENABLED" : "DISABLED", strategy, ticker);
            if (enabled) {
                if (!controllers_.contains(ticker)) {
                    controllers_[ticker] = std::make_unique<TickerController>(ticker, *signal_bus_, universe_, *cluster_mgr_);
                    books_for_executor_[ticker] = controllers_[ticker]->book.get();
                    feed_->add_listener(ticker, controllers_[ticker].get());
                    feed_->subscribe_ticker(ticker);  // also sends funding_subscribe + mark_price_subscribe
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

        for (auto& [ticker, ctrl] : controllers_) {
            auto funding = feed_->get_funding(ticker);
            if (funding && last_funding_times_[ticker] != funding->next_funding_time) {
                risk_manager_->update_funding_time(ticker, funding->next_funding_time);
                last_funding_times_[ticker] = funding->next_funding_time;
            }
            
            auto frame = ctrl->tick(now);
            strategy_engine_->on_frame(frame);
        }
        
        strategy_engine_->tick(now);
        executor_->tick(now);

        // Apply closed trades → update equity and write journal
        for (const auto& ct : executor_->pop_closed_trades()) {
            account_state_.realized_pnl_today_usd += ct.pnl_usd;
            account_state_.equity_usd             += ct.pnl_usd;
            account_state_.free_balance_usd       += ct.pnl_usd;
            TradeJournal::Entry je;
            je.plan          = ct.plan;
            je.pnl_usd       = ct.pnl_usd;
            je.exit_price    = ct.exit_price;
            je.cause_of_exit = ct.reason;
            journal_.log_entry(je);
        }
        account_state_.unrealized_pnl_usd = executor_->unrealized_pnl();

        if (now - last_persist_ > std::chrono::seconds(10)) {
            persister_->save({account_state_, {}, last_reset_day_, false, ""});
            last_persist_ = now;
        }

        DashboardServer::State dash_state;
        dash_state.account = account_state_;
        dash_state.recent_journal = journal_.get_recent_entries(20);
        dash_state.open_trades = executor_->get_active_trades();
        dash_state.kill_switch_active = kill_switch_->is_triggered();
        dash_state.version = Config::get<std::string>("app.version");
        dash_state.signal_counts = signal_counts_;
        for (const auto& ticker : universe_.active()) {
            DashboardServer::State::UniverseRow row;
            row.ticker     = ticker;
            row.strategies = universe_.enabled_strategies(ticker);
            row.boosted    = universe_.is_boosted(ticker, now);
            row.mark_price = feed_->get_mark_price(ticker);
            dash_state.universe_rows.push_back(std::move(row));
        }
        dash_state.recent_signals.assign(recent_signals_.begin(), recent_signals_.end());
        dashboard_->update_state(dash_state);
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
    std::unique_ptr<PaperExecutor> executor_;
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

