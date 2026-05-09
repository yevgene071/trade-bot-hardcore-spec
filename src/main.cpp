#include "logger/Logger.hpp"
#include "logger/TradeJournal.hpp"
#include "config/Config.hpp"
#include "control/KillSwitch.hpp"
#include "control/ClockDriftMonitor.hpp"
#include "control/SntpClient.hpp"
#include "control/TickerController.hpp"
#include "transport/MarketDataFeed.hpp"
#include "transport/DumpRecorder.hpp"
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
        clock_monitor_ = std::make_unique<ClockDriftMonitor>(ntp, ClockDriftMonitor::Config{});
        clock_monitor_->start();

        auto& ext_ioc = ExternalIoContext::instance();
        funding_client_ = std::make_shared<BinanceFundingClient>(std::make_shared<CurlHttpClient>());
        funding_client_->start_polling(ext_ioc.context(), std::chrono::seconds(60));
        ExternalFeedRegistry::instance().register_feed(FeedKind::Funding, funding_client_);
        ext_ioc.start();

        const auto dashboard_addr = Config::get_or<std::string>("dashboard.bind_address", "127.0.0.1");
        const auto dashboard_port = static_cast<uint16_t>(Config::get_or<int64_t>("dashboard.port", 8080));
        const auto dashboard_token = Config::get_or<std::string>("dashboard.auth_token", std::string{});
        dashboard_ = std::make_unique<DashboardServer>(ioc_, dashboard_addr, dashboard_port, dashboard_token);
        dashboard_->start();

        const auto metrics_port = static_cast<uint16_t>(Config::get_or<int64_t>("metrics.port", 9090));
        const auto metrics_addr = Config::get_or<std::string>("metrics.bind_address", "127.0.0.1");
        const auto metrics_token = Config::get_or<std::string>("metrics.auth_token", std::string{});
        metrics_exporter_ = std::make_unique<MetricsExporter>(ioc_, metrics_addr, metrics_port, metrics_token);
        metrics_exporter_->start();

        signal_bus_ = std::make_unique<SignalBus>();
        signal_bus_->subscribe([&](const Signal& s) {
            MetricsRegistry::instance().counter_inc("trade_bot_signals_total", {{"kind", std::to_string(static_cast<int>(s.kind))}});
        });

        strategy_engine_ = std::make_unique<StrategyEngine>(*signal_bus_);
        risk_manager_ = std::make_unique<RiskManager>(universe_, news_);
        
        auto persisted = persister_->load();
        if (persisted) {
            account_state_ = persisted->account_state;
            last_reset_day_ = persisted->last_reset_day_utc;
        } else {
            account_state_.equity_usd = 10000.0;
            last_reset_day_ = TradingDay::current_date_utc();
        }
        // In paper mode there are no exchange balance updates, so seed free_balance
        // from equity so the dashboard shows a non-zero figure.
        if (account_state_.free_balance_usd == 0.0 && account_state_.equity_usd > 0.0)
            account_state_.free_balance_usd = account_state_.equity_usd;

        auto http = std::make_shared<CurlHttpClient>();
        auto ws = std::make_shared<BeastWsClient>(ioc_);
        
        MetaScalpDiscovery discovery(http);
        auto port = discovery.discover();
        if (!port) throw std::runtime_error("MetaScalp not found");

        feed_ = std::make_unique<MarketDataFeed>(ws, 1);
        feed_->set_record_tap([this](const nlohmann::json& msg, int64_t ts_ns) {
            dump_recorder_.record(msg, ts_ns);
        });
        dashboard_->set_recorder(&dump_recorder_);
        cluster_client_ = std::make_unique<ClusterSnapshotClient>(*http, "http://localhost:" + std::to_string(*port), 1);
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
            if (enabled) {
                if (!controllers_.contains(ticker)) {
                    controllers_[ticker] = std::make_unique<TickerController>(ticker, *signal_bus_, universe_, *cluster_mgr_);
                    books_for_executor_[ticker] = controllers_[ticker]->book.get();
                    feed_->subscribe_ticker(ticker);
                    funding_client_->add_ticker(ticker);
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
        // Fix for #141: Actually discovery already provides the port, but ws://127.0.0.1 is still hardcoded.
        // In a real scenario, discovery might provide the full endpoint.
        // For now, let's ensure we use the discovered port consistently.
        feed_->start();
        
        auto initial_universe = Config::get_or<std::vector<std::string>>("universe.initial_pool", {"BTCUSDT", "ETHUSDT"});
        universe_.refresh_pool(initial_universe);
        universe_.refresh_affinity();
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
            auto funding = funding_client_->get_funding(ticker);
            if (funding) {
                if (last_funding_times_[ticker] != funding->next_funding_time) {
                    risk_manager_->update_funding_time(ticker, funding->next_funding_time);
                    last_funding_times_[ticker] = funding->next_funding_time;
                }
            }
            
            auto frame = ctrl->tick(now);
            strategy_engine_->on_frame(frame);
        }
        
        strategy_engine_->tick(now);
        executor_->tick(now);

        if (now - last_persist_ > std::chrono::seconds(10)) {
            persister_->save({account_state_, {}, last_reset_day_, false, ""});
            last_persist_ = now;
        }

        DashboardServer::State dash_state;
        dash_state.account = account_state_;
        dash_state.open_trades = executor_->get_active_trades();
        dash_state.kill_switch_active = kill_switch_->is_triggered();
        dash_state.version = Config::get<std::string>("app.version");
        dashboard_->update_state(dash_state);
    }

private:
    boost::asio::io_context& ioc_;
    boost::asio::steady_timer timer_;
    
    TickerUniverse universe_;
    NewsCalendar news_;
    AccountState account_state_;
    std::string last_reset_day_;
    std::chrono::system_clock::time_point last_persist_;

    DumpRecorder dump_recorder_;
    KillSwitch* kill_switch_;
    std::unique_ptr<ClockDriftMonitor> clock_monitor_;
    std::shared_ptr<BinanceFundingClient> funding_client_;
    std::unique_ptr<DashboardServer> dashboard_;
    std::unique_ptr<MetricsExporter> metrics_exporter_;
    std::unique_ptr<SignalBus> signal_bus_;
    std::unique_ptr<StrategyEngine> strategy_engine_;
    std::unique_ptr<RiskManager> risk_manager_;
    std::unique_ptr<MarketDataFeed> feed_;
    std::unique_ptr<ClusterSnapshotClient> cluster_client_;
    std::unique_ptr<ClusterSnapshotManager> cluster_mgr_;
    std::unique_ptr<PaperExecutor> executor_;
    std::unique_ptr<AccountStatePersister> persister_;

    std::map<Ticker, std::unique_ptr<TickerController>> controllers_;
    std::map<Ticker, const OrderBook*> books_for_executor_;
    std::map<Ticker, std::chrono::system_clock::time_point> last_funding_times_;
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
