#include "logger/Logger.hpp"
#include "logger/TradeJournal.hpp"
#include "config/Config.hpp"
#include "control/KillSwitch.hpp"
#include "control/ClockDriftMonitor.hpp"
#include "control/SntpClient.hpp"
#include "control/TickerController.hpp"
#include "transport/MarketDataFeed.hpp"
#include "transport/MetaScalpCodec.hpp"
#include "transport/MetaScalpDiscovery.hpp"
#include "transport/OrderGateway.hpp"
#include "transport/BeastWsClient.hpp"
#include "transport/CurlHttpClient.hpp"
#include "transport/FinresHandler.hpp"
#include "transport/ClusterSnapshotClient.hpp"
#include "transport/external/BinanceFundingClient.hpp"
#include "transport/external/ExternalIoContext.hpp"
#include "transport/external/FeedStalenessMonitor.hpp"
#include "marketdata/ClusterSnapshot.hpp"
#include "universe/TickerUniverse.hpp"
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
#include <filesystem>
#include <iostream>
#include <memory>
#include <thread>
#include <map>
#include <boost/asio/io_context.hpp>

#include "control/DashboardServer.hpp"

using namespace trade_bot;

int main() {
    try {
        Logger::init();
        TradeJournal journal;
        AccountStatePersister persister("journal/account_state.json");
        FinresHandler finres_handler;

        if (std::filesystem::exists("./killswitch")) {
            std::cerr << "Kill-switch file present, remove it to run" << std::endl;
            return 42;
        }

        if (!std::filesystem::exists("config.toml")) {
            LOG_CRITICAL("Config 'config.toml' not found");
            return 2;
        }
        Config::load("config.toml");

        LOG_INFO("trade_bot {} starting...", Config::get<std::string>("app.version"));

        auto& kill_switch = KillSwitch::instance();
        kill_switch.start();

        // T0-CLOCK
        auto ntp = std::make_shared<SntpClient>();
        ClockDriftMonitor clock_monitor(ntp, ClockDriftMonitor::Config{});
        clock_monitor.start();

        // External Feeds
        auto& ext_ioc = ExternalIoContext::instance();
        auto funding_client = std::make_shared<BinanceFundingClient>(std::make_shared<CurlHttpClient>());
        funding_client->start_polling(ext_ioc.context(), std::chrono::seconds(60));
        ExternalFeedRegistry::instance().register_feed(FeedKind::Funding, funding_client);
        ext_ioc.start();

        boost::asio::io_context ioc;
        DashboardServer dashboard(ioc, "0.0.0.0", 8080);
        dashboard.start();

        auto signal_to_string = [](SignalKind k) {
            switch(k) {
                case SignalKind::DensityDetected: return "DensityDetected";
                case SignalKind::DensityRemoved: return "DensityRemoved";
                case SignalKind::DensityEating: return "DensityEating";
                case SignalKind::IcebergSuspected: return "IcebergSuspected";
                case SignalKind::TapeBurst: return "TapeBurst";
                case SignalKind::TapeFade: return "TapeFade";
                case SignalKind::TapeFlush: return "TapeFlush";
                case SignalKind::LevelFormed: return "LevelFormed";
                case SignalKind::LevelApproach: return "LevelApproach";
                case SignalKind::LevelRejection: return "LevelRejection";
                case SignalKind::LevelBreak: return "LevelBreak";
                case SignalKind::LeaderMove: return "LeaderMove";
                default: return "Unknown";
            }
        };

        std::map<std::string, int> signal_counts;
        SignalBus signal_bus;
        signal_bus.subscribe([&](const Signal& s) {
            signal_counts[signal_to_string(s.kind)]++;
        });
        TickerUniverse universe;
        StrategyEngine strategy_engine(signal_bus);
        
        // T4-RISK
        NewsCalendar news;
        RiskManager risk_manager(universe, news);
        
        // Load persisted state
        auto persisted = persister.load();
        AccountState account_state;
        std::string last_reset_day;

        if (persisted) {
            account_state = persisted->account_state;
            last_reset_day = persisted->last_reset_day_utc;
            LOG_INFO("RiskManager: restored state, equity: ${}, last reset: {}", 
                     account_state.equity_usd, last_reset_day);
        } else {
            account_state.starting_equity_usd = 10000.0; // dummy
            account_state.equity_usd = 10000.0;
            account_state.free_balance_usd = 10000.0;
            last_reset_day = TradingDay::current_date_utc();
        }

        // Transport
        auto http = std::make_shared<CurlHttpClient>();
        auto ws = std::make_shared<BeastWsClient>(ioc);
        
        MetaScalpDiscovery discovery(http);
        auto port = discovery.discover();
        if (!port) {
            LOG_CRITICAL("MetaScalp not found");
            return 1;
        }

        int connection_id = 1; // Default connection
        MarketDataFeed feed(ws, connection_id);
        
        ClusterSnapshotClient cluster_client(*http, "http://localhost:" + std::to_string(*port), connection_id);
        ClusterSnapshotManager cluster_mgr(cluster_client);
        cluster_mgr.start();

        // Controllers
        std::map<Ticker, std::unique_ptr<TickerController>> controllers;
        std::map<Ticker, const OrderBook*> books_for_executor;
        PaperExecutor executor(books_for_executor);
        
        strategy_engine.set_on_plan([&](const TradePlan& plan) {
            auto decision = risk_manager.evaluate(plan, account_state);
            if (decision.accepted) {
                TradePlan accepted_plan = plan;
                accepted_plan.size_coin = decision.adjusted_size_coin;
                accepted_plan.risk_usd = decision.risk_usd;
                executor.submit(accepted_plan);
            } else {
                LOG_WARN("RiskManager: REJECTED plan for {} {}: {}", 
                         plan.strategy_name, plan.ticker, decision.details);
            }
        });

        universe.set_affinity_change_handler([&](const Ticker& ticker, const std::string& strategy, bool enabled) {
            if (enabled) {
                if (!controllers.contains(ticker)) {
                    controllers[ticker] = std::make_unique<TickerController>(ticker, signal_bus, universe, cluster_mgr);
                    books_for_executor[ticker] = controllers[ticker]->book.get();
                    feed.subscribe_ticker(ticker);
                }
                if (strategy == "bounce") strategy_engine.add_strategy(std::make_unique<BounceFromDensity>(ticker));
                else if (strategy == "breakout") strategy_engine.add_strategy(std::make_unique<BreakoutEatThrough>(ticker));
                else if (strategy == "leaderlag") strategy_engine.add_strategy(std::make_unique<LeaderLag>(ticker));
            }
        });

        universe.register_strategy("bounce", [](const Ticker&){ return true; });
        universe.register_strategy("breakout", [](const Ticker&){ return true; });

        struct MDListener : public IMarketDataListener {
            std::map<Ticker, std::unique_ptr<TickerController>>& ctrls;
            FinresHandler& finres;
            MDListener(std::map<Ticker, std::unique_ptr<TickerController>>& c, FinresHandler& f) 
                : ctrls(c), finres(f) {}
            void on_trade(const Ticker& ticker, const Trade& t) override { if (ctrls.contains(ticker)) ctrls[ticker]->on_trade(t); }
            void on_orderbook_snapshot(const OrderBookSnapshot& s) override { if (ctrls.contains(s.ticker)) ctrls[s.ticker]->book->apply_snapshot(s); }
            void on_orderbook_update(const OrderBookUpdate& u) override { if (ctrls.contains(u.ticker)) ctrls[u.ticker]->on_book_update(u); }
            void on_order_update(const OrderUpdate&) override {}
            void on_position_update(const PositionUpdate&) override {}
            void on_balance_update(const BalanceUpdate&) override {}
            void on_finres_update(const FinresUpdate& u) override { finres.handle_update(u); }
            void on_error(const std::string& msg) override { LOG_ERROR("MarketDataFeed error: {}", msg); }
        } md_listener{controllers, finres_handler};
        feed.add_listener(&md_listener);

        ws->connect("ws://127.0.0.1:" + std::to_string(*port) + "/ws");
        feed.start();
        universe.refresh_pool({"BTCUSDT", "ETHUSDT"});
        universe.refresh_affinity();

        LOG_INFO("System wired, starting main loop...");

        auto last_persist = std::chrono::system_clock::now();
        auto last_stale_check = std::chrono::system_clock::now();

        std::map<std::string, FeedStalenessMonitor::Config> stale_cfgs;
        stale_cfgs["BinanceFunding"] = {std::chrono::seconds(120), std::chrono::seconds(1000), false};

        while (!kill_switch.is_triggered()) {
            ioc.poll();
            auto now = std::chrono::system_clock::now();
            
            // Check day reset
            if (TradingDay::is_new_day(last_reset_day)) {
                LOG_INFO("TradingDay: new day detected, resetting equity baseline");
                account_state.starting_equity_usd = account_state.equity_usd;
                finres_handler.reset_day_start();
                account_state.realized_pnl_today_usd = 0.0;
                last_reset_day = TradingDay::current_date_utc();
            }

            // Update PnL from FinresHandler
            account_state.realized_pnl_today_usd = finres_handler.get_realized_pnl(connection_id);

            // Update funding times in RiskManager
            for (const auto& [ticker, ctrl] : controllers) {
                auto funding = funding_client->get_funding(ticker);
                if (funding) {
                    risk_manager.update_funding_time(ticker, funding->next_funding_time);
                }
            }

            for (auto& [ticker, ctrl] : controllers) {
                auto frame = ctrl->tick(now);
                strategy_engine.on_frame(frame);
            }
            
            strategy_engine.tick(now);
            executor.tick(now);

            // Periodic persist
            if (now - last_persist > std::chrono::seconds(10)) {
                persister.save({account_state, {}, last_reset_day, false, ""});
                last_persist = now;
            }

            // Stale check
            if (now - last_stale_check > std::chrono::seconds(30)) {
                FeedStalenessMonitor::check_all(stale_cfgs);
                last_stale_check = now;
            }

            // Update Dashboard
            DashboardServer::State dash_state;
            dash_state.account = account_state;
            dash_state.open_trades = executor.get_active_trades();
            dash_state.recent_journal = journal.get_recent_entries(20);
            dash_state.signal_counts = signal_counts;
            dash_state.kill_switch_active = kill_switch.is_triggered();
            dash_state.version = Config::get<std::string>("app.version");
            dashboard.update_state(dash_state);

            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        LOG_INFO("Shutting down...");
        cluster_mgr.stop();
        ext_ioc.stop();
        clock_monitor.stop();
        kill_switch.stop();

    } catch (const std::exception& e) {
        std::cerr << "Fatal error: " << e.what() << std::endl;
        return 1;
    }
    return 0;
}
