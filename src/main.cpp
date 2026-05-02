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
#include "universe/TickerUniverse.hpp"
#include "strategy/StrategyEngine.hpp"
#include "strategy/BounceFromDensity.hpp"
#include "strategy/BreakoutEatThrough.hpp"
#include "strategy/LeaderLag.hpp"
#include "executor/PaperExecutor.hpp"

#include <chrono>
#include <filesystem>
#include <iostream>
#include <memory>
#include <thread>
#include <map>
#include <boost/asio/io_context.hpp>

using namespace trade_bot;

int main() {
    try {
        Logger::init();
        TradeJournal journal;

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

        // T3-INTEGRATION
        SignalBus signal_bus;
        TickerUniverse universe;
        StrategyEngine strategy_engine(signal_bus);
        
        // Transport
        boost::asio::io_context ioc;
        auto http = std::make_shared<CurlHttpClient>();
        auto ws = std::make_shared<BeastWsClient>(ioc);
        MarketDataFeed feed(ws, 1);
        
        ClusterSnapshotClient cluster_client(*http, "http://localhost", 1);
        ClusterSnapshotManager cluster_mgr(cluster_client);
        cluster_mgr.start();

        // Per-ticker controllers
        std::map<Ticker, std::unique_ptr<TickerController>> controllers;
        
        // Books map for executor
        std::map<Ticker, const OrderBook*> books_for_executor;

        // Paper Executor
        PaperExecutor executor(books_for_executor);
        
        strategy_engine.set_on_plan([&executor](const TradePlan& plan) {
            executor.submit(plan);
        });

        universe.set_affinity_change_handler([&](const Ticker& ticker, const std::string& strategy, bool enabled) {
            if (enabled) {
                if (!controllers.contains(ticker)) {
                    controllers[ticker] = std::make_unique<TickerController>(ticker, signal_bus, universe, cluster_mgr);
                    books_for_executor[ticker] = controllers[ticker]->book.get();
                    feed.subscribe_ticker(ticker);
                }
                
                if (strategy == "bounce") {
                    strategy_engine.add_strategy(std::make_unique<BounceFromDensity>(ticker));
                } else if (strategy == "breakout") {
                    strategy_engine.add_strategy(std::make_unique<BreakoutEatThrough>(ticker));
                } else if (strategy == "leaderlag") {
                    strategy_engine.add_strategy(std::make_unique<LeaderLag>(ticker));
                }
                LOG_INFO("Strategy {} enabled for {}", strategy, ticker);
            }
        });

        universe.register_strategy("bounce", [](const Ticker&){ return true; });
        universe.register_strategy("breakout", [](const Ticker&){ return true; });

        struct MDListener : public IMarketDataListener {
            std::map<Ticker, std::unique_ptr<TickerController>>& ctrls;
            explicit MDListener(std::map<Ticker, std::unique_ptr<TickerController>>& c) : ctrls(c) {}
            void on_trade(const Ticker& ticker, const Trade& t) override { if (ctrls.contains(ticker)) ctrls[ticker]->on_trade(t); }
            void on_orderbook_snapshot(const OrderBookSnapshot& s) override { if (ctrls.contains(s.ticker)) ctrls[s.ticker]->book->apply_snapshot(s); }
            void on_orderbook_update(const OrderBookUpdate& u) override { if (ctrls.contains(u.ticker)) ctrls[u.ticker]->on_book_update(u); }
            void on_order_update(const OrderUpdate&) override {}
            void on_position_update(const PositionUpdate&) override {}
            void on_balance_update(const BalanceUpdate&) override {}
            void on_error(const std::string& msg) override { LOG_ERROR("MarketDataFeed error: {}", msg); }
        } md_listener{controllers};
        feed.add_listener(&md_listener);

        universe.refresh_pool({"BTCUSDT", "ETHUSDT"});
        universe.refresh_affinity();

        LOG_INFO("System wired, starting main loop...");

        while (!kill_switch.is_triggered()) {
            ioc.poll();
            auto now = std::chrono::system_clock::now();
            
            for (auto& [ticker, ctrl] : controllers) {
                auto frame = ctrl->tick(now);
                strategy_engine.on_frame(frame);
            }
            
            strategy_engine.tick(now);
            executor.tick(now);

            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        LOG_INFO("Shutting down...");
        cluster_mgr.stop();
        clock_monitor.stop();
        kill_switch.stop();

    } catch (const std::exception& e) {
        std::cerr << "Fatal error: " << e.what() << std::endl;
        return 1;
    }
    return 0;
}
