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
#include "universe/OrderbookSettingsLoader.hpp"
#include "strategy/StrategyEngine.hpp"
#include "strategy/BounceFromDensity.hpp"
#include "strategy/BreakoutEatThrough.hpp"
#include "strategy/LeaderLag.hpp"
#include "executor/PaperExecutor.hpp"
#include "executor/LiveExecutor.hpp"
#include "executor/StartupRecovery.hpp"
#include "risk/RiskManager.hpp"
#include "risk/AccountState.hpp"
#include "risk/AccountStatePersister.hpp"
#include "risk/TradingDay.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <optional>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <spdlog/fmt/ranges.h>
#include <iostream>
#include <memory>
#include <set>
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
        , pool_fallback_timer_(ioc)
        , last_reset_day_(TradingDay::current_date_utc())
        , last_persist_(std::chrono::system_clock::now())
        , last_dash_update_(std::chrono::system_clock::now())
        , last_slow_dash_update_(std::chrono::system_clock::time_point{})  // force first slow tick
        , kill_switch_(&KillSwitch::instance()) {}

    ~BotApp() {
        dashboard_ioc_.stop();
        if (dashboard_thread_.joinable()) {
            dashboard_thread_.join();
        }
    }

    void run() {
        init_components();
        schedule_tick();
    }

private:
    void init_components() {
        if (std::filesystem::exists("./killswitch")) {
            throw std::runtime_error("Kill-switch file present");
        }

        if (!std::filesystem::exists("config.toml")) {
            throw std::runtime_error("Config 'config.toml' not found");
        }
        Config::load("config.toml");
        
        const auto log_path = Config::get_or<std::string>("logger.path", "logs/trade_bot.log");
        const auto log_level = Config::get_or<std::string>("logger.level", "info");
        Logger::init(log_path, log_level);

        persister_ = std::make_unique<AccountStatePersister>("journal/account_state.json");
        LOG_INFO("trade_bot {} starting...", Config::get<std::string>("app.version"));

        kill_switch_ = &KillSwitch::instance();
        kill_switch_->start();

        // T1-UNIVERSE: Register strategies BEFORE pool formation with real affinity checks.
        // Exchange volume multiplier is applied so MEXC/Bybit thresholds are realistic.
        // We capture exchange_volume_mult by value here — it's set further down.
        // To break the circular dependency, we use a shared_ptr indirection.
        auto exchange_mult = std::make_shared<double>(1.0);

        double bounce_min_vol = Config::get_or<double>("universe.affinity.bounce.min_volume_24h_usd", 100000000.0);
        double bounce_max_spread = Config::get_or<double>("universe.affinity.bounce.max_avg_spread_bps", 3.0);
        // Variant B: no stats → trust screener (passes_filter_ already gates screener coins).
        // This fixes Bug #1: without set_stats_lookup(), get_stats()→nullopt always,
        // causing ALL strategies to stay disabled → no controllers → no market data subscription.
        universe_.register_strategy("bounce", [this, bounce_min_vol, bounce_max_spread, exchange_mult](const Ticker& t) {
            auto stats = universe_.get_stats(t);
            if (!stats) return true;  // no data yet → rely on screener gate in passes_filter_
            double scaled_min_vol = bounce_min_vol * (*exchange_mult);
            return stats->volume_24h_usd >= scaled_min_vol && stats->avg_spread_bps <= bounce_max_spread;
        });

        double breakout_min_vol = Config::get_or<double>("universe.affinity.breakout.min_volume_24h_usd", 100000000.0);
        double breakout_max_spread = Config::get_or<double>("universe.affinity.breakout.max_avg_spread_bps", 4.0);
        universe_.register_strategy("breakout", [this, breakout_min_vol, breakout_max_spread, exchange_mult](const Ticker& t) {
            auto stats = universe_.get_stats(t);
            if (!stats) return true;
            double scaled_min_vol = breakout_min_vol * (*exchange_mult);
            return stats->volume_24h_usd >= scaled_min_vol && stats->avg_spread_bps <= breakout_max_spread;
        });

        double leaderlag_min_vol = Config::get_or<double>("universe.affinity.leaderlag.min_volume_24h_usd", 50000000.0);
        double leaderlag_max_spread = Config::get_or<double>("universe.affinity.leaderlag.max_avg_spread_bps", 3.0);
        universe_.register_strategy("leaderlag", [this, leaderlag_min_vol, leaderlag_max_spread, exchange_mult](const Ticker& t) {
            auto stats = universe_.get_stats(t);
            if (!stats) return true;
            double scaled_min_vol = leaderlag_min_vol * (*exchange_mult);
            return stats->volume_24h_usd >= scaled_min_vol && stats->avg_spread_bps <= leaderlag_max_spread;
        });

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
        
        RiskManager::Config rm_cfg;
        rm_cfg.max_daily_loss_pct        = Config::get_or<double>("risk.max_daily_loss_pct", 3.0);
        rm_cfg.max_concurrent_positions  = static_cast<int>(Config::get_or<int64_t>("risk.max_concurrent_positions", 3));
        rm_cfg.max_per_trade_risk_pct    = Config::get_or<double>("risk.max_per_trade_risk_pct", 0.5);
        rm_cfg.max_position_value_pct    = Config::get_or<double>("risk.max_position_value_pct", 10.0);
        rm_cfg.max_leverage              = static_cast<int>(Config::get_or<int64_t>("risk.max_leverage", 5));
        rm_cfg.news_blackout_min         = static_cast<int>(Config::get_or<int64_t>("risk.news_blackout_min", 5));
        rm_cfg.news_calendar_check_min   = static_cast<int>(Config::get_or<int64_t>("risk.news_calendar_check_min", 60));
        rm_cfg.news_calendar_require_fresh = Config::get_or<bool>("risk.news_calendar_require_fresh", false);
        rm_cfg.funding_blackout_pre_sec  = static_cast<int>(Config::get_or<int64_t>("risk.funding_blackout_pre_sec", 30));
        rm_cfg.funding_blackout_post_sec = static_cast<int>(Config::get_or<int64_t>("risk.funding_blackout_post_sec", 30));
        rm_cfg.min_stop_bps              = Config::get_or<double>("risk.min_stop_bps", 3.0);
        rm_cfg.max_stop_bps              = Config::get_or<double>("risk.max_stop_bps", 20.0);
        rm_cfg.min_rr_ratio              = Config::get_or<double>("risk.min_rr_ratio", 1.0);
        rm_cfg.max_positions_per_ticker  = static_cast<int>(Config::get_or<int64_t>("risk.max_positions_per_ticker", 1));
        
        risk_manager_ = std::make_unique<RiskManager>(universe_, news_, rm_cfg);
        
        auto persisted = persister_->load();
        if (persisted) {
            account_state_ = persisted->account_state;
            last_reset_day_ = persisted->last_reset_day_utc;
            // Guard: persisted equity may be 0 if a previous bug zeroed it out.
            // Recover from starting_equity_usd which is always persisted correctly.
            if (account_state_.equity_usd <= 0.0 && account_state_.starting_equity_usd > 0.0) {
                account_state_.equity_usd      = account_state_.starting_equity_usd;
                account_state_.free_balance_usd = account_state_.starting_equity_usd;
                LOG_WARN("Persisted equity_usd was 0 — recovered from starting_equity_usd={}",
                         account_state_.starting_equity_usd);
            }
            LOG_INFO("Loaded account state: equity={}, realized_pnl={}, starting_equity={}",
                     account_state_.equity_usd, account_state_.realized_pnl_today_usd,
                     account_state_.starting_equity_usd);
        } else {
            account_state_.equity_usd          = 10000.0;
            account_state_.starting_equity_usd = 10000.0;
            account_state_.free_balance_usd    = 10000.0;
            last_reset_day_ = TradingDay::current_date_utc();
            LOG_INFO("No persisted state found, initialized with 10000.0");
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

        http_client_ = std::make_shared<CurlHttpClient>();
        http_client_->set_timeout_ms(15000); // T4-RECOVERY: 15s timeout for heavy initialization requests
        auto ws = std::make_shared<BeastWsClient>(ioc_);
        
        MetaScalpDiscovery discovery(http_client_);
        auto port = discovery.discover();
        if (!port) throw std::runtime_error("MetaScalp not found");

        auto gateway = std::make_shared<OrderGateway>(http_client_);
        gateway->set_port(*port);
        
        int connection_id = 1; // Default
        std::string connection_name = "Unknown";
        double exchange_volume_mult = 1.0;

        // Config override for exchange volume multiplier (takes priority over auto-detect).
        std::optional<double> cfg_exchange_mult;
        if (Config::has("universe.pool.exchange_volume_multiplier")) {
            try {
                cfg_exchange_mult = Config::get<double>("universe.pool.exchange_volume_multiplier");
            } catch (const ConfigError&) {
                LOG_WARN("[Universe] exchange_volume_multiplier has wrong type, using auto-detect");
            }
        }

        try {
            auto connections = gateway->get_connections();
            for (const auto& conn : connections) {
                if (conn.name.find("Futures") != std::string::npos) {
                    connection_id = conn.id;
                    connection_name = conn.name;
                    LOG_INFO("Selected connection {}: {}", connection_id, conn.name);
                    break;
                }
            }

            if (cfg_exchange_mult) {
                exchange_volume_mult = *cfg_exchange_mult;
                LOG_INFO("[Universe] Exchange volume multiplier overridden by config: {:.3f}x",
                         exchange_volume_mult);
            } else {
                // Auto-detect exchange for volume-threshold scaling.
                // MEXC volumes are ~10x lower than Binance → threshold scaled accordingly.
                std::string name_lower = connection_name;
                std::transform(name_lower.begin(), name_lower.end(), name_lower.begin(), ::tolower);
                if (name_lower.find("mexc") != std::string::npos) {
                    exchange_volume_mult = 0.1;
                    LOG_INFO("[Universe] MEXC detected — volume thresholds scaled by 0.1x");
                } else if (name_lower.find("bybit") != std::string::npos) {
                    exchange_volume_mult = 0.5;
                    LOG_INFO("[Universe] Bybit detected — volume thresholds scaled by 0.5x");
                }
            }
        } catch (const std::exception& e) {
            LOG_WARN("Failed to get connections, using default ID 1: {}", e.what());
        }

        // Feed back into the strategy affinity lambdas.
        *exchange_mult = exchange_volume_mult;

        // T4-RISK: Sync account state with MetaScalp via FinresHandler
        finres_handler_ = std::make_unique<FinresHandler>();
        feed_ = std::make_unique<MarketDataFeed>(ws, connection_id);
        feed_->add_listener(finres_handler_.get());
        m_connection_id_for_tick = connection_id;
        feed_->set_record_tap([this](const nlohmann::json& msg, int64_t ts_ns) {
            dump_recorder_.record(msg, ts_ns);
        });
        dashboard_->set_recorder(&dump_recorder_);
        cluster_client_ = std::make_unique<ClusterSnapshotClient>(*http_client_, "http://localhost:" + std::to_string(*port), connection_id);
        cluster_mgr_ = std::make_unique<ClusterSnapshotManager>(*cluster_client_);
        cluster_mgr_->start();

        // T1-CALIB: Orderbook settings loader for per-ticker LargeAmountUsd calibration.
        ob_settings_loader_ = std::make_unique<OrderbookSettingsLoader>(
            *http_client_, "http://localhost:" + std::to_string(*port), connection_id);

        // T4-EXECUTOR: Respect mode config (paper vs live)
        const std::string mode = Config::get_or<std::string>("executor.mode", "paper");
        live_executor_ = (mode == "live");
        if (live_executor_) {
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

                LOG_INFO("[Strategy] SUBMITTING plan for {}: {} @ {}, size={}",
                         plan.ticker, plan.side == Side::Buy ? "BUY" : "SELL",
                         accepted_plan.entry_price, accepted_plan.size_coin);
                executor_->submit(accepted_plan);
                strategy_engine_->notify_plan_accepted(accepted_plan);
            } else {
                LOG_INFO("[Risk] Plan REJECTED for {}: {} ({})", 
                         plan.ticker, static_cast<int>(decision.reason), decision.details);
                
                // T4-RECOVERY: Signal the strategy to reset its active plan
                strategy_engine_->reset_strategy_plan(plan.ticker, plan.strategy_name.c_str());
            }
        });

        // Wire close-callback: StrategyEngine → Executor (LeaderLag post-entry monitoring)
        strategy_engine_->set_close_callback([&](const Ticker& ticker, const FixedString<32>& reason) {
            LOG_INFO("[Strategy] CLOSE requested for {} — {}", ticker, reason);
            executor_->close_trade(ticker, reason);
        });

        // Build strategy configs from toml (post-entry invalidation params + existing)
        BounceFromDensity::Config bounce_cfg;
        bounce_cfg.entry_offset_bps = Config::get_or<double>("strategies.bounce.entry_offset_bps", 3.0);
        bounce_cfg.stop_buffer_bps = Config::get_or<double>("strategies.bounce.stop_buffer_bps", 5.0);
        bounce_cfg.max_spread_bps = Config::get_or<double>("strategies.bounce.max_spread_bps", 3.0);
        bounce_cfg.tp1_size_ratio = Config::get_or<double>("strategies.bounce.tp1_size_ratio", 0.5);
        bounce_cfg.burst_contra_exit_sec = std::chrono::seconds(
            Config::get_or<int64_t>("strategies.bounce.burst_contra_exit_sec", 5));
        bounce_cfg.min_density_age_ms = std::chrono::milliseconds(
            Config::get_or<int64_t>("strategies.bounce.min_density_age_ms", 5000));

        BreakoutEatThrough::Config breakout_cfg;
        breakout_cfg.aggressive_offset_bps = Config::get_or<double>("strategies.breakout.aggressive_offset_bps", 2.0);
        breakout_cfg.stop_buffer_bps = Config::get_or<double>("strategies.breakout.stop_buffer_bps", 5.0);
        breakout_cfg.tp1_size_ratio = Config::get_or<double>("strategies.breakout.tp1_size_ratio", 0.5);
        breakout_cfg.min_tape_aggression = Config::get_or<double>("strategies.breakout.min_tape_aggression", 0.3);
        breakout_cfg.min_relative_volume = Config::get_or<double>("strategies.breakout.min_relative_volume", 1.5);
        breakout_cfg.max_resistance_cluster_ratio = Config::get_or<double>("strategies.breakout.max_resistance_cluster_ratio", 0.7);
        breakout_cfg.fade_contra_exit_sec = std::chrono::seconds(
            Config::get_or<int64_t>("strategies.breakout.fade_contra_exit_sec", 5));
        breakout_cfg.leader_contra_exit_sec = std::chrono::seconds(
            Config::get_or<int64_t>("strategies.breakout.leader_contra_exit_sec", 5));
        breakout_cfg.leader_exit_contra_pct = Config::get_or<double>("strategies.breakout.leader_exit_contra_pct", 0.15);

        LeaderLag::Config leaderlag_cfg;
        leaderlag_cfg.stop_distance_bps = Config::get_or<double>("strategies.leaderlag.stop_distance_bps", 8.0);
        leaderlag_cfg.tp1_size_ratio = Config::get_or<double>("strategies.leaderlag.tp1_size_ratio", 0.6);
        leaderlag_cfg.correlation_exit_threshold = Config::get_or<double>("strategies.leaderlag.correlation_exit_threshold", 0.3);
        leaderlag_cfg.leader_exit_reversal_bps = Config::get_or<double>("strategies.leaderlag.leader_exit_reversal_bps", 5.0);
        leaderlag_cfg.swing_lookback_ticks = static_cast<int>(
            Config::get_or<int64_t>("strategies.leaderlag.swing_lookback_ticks", 30));

        universe_.set_affinity_change_handler([&, bounce_cfg, breakout_cfg, leaderlag_cfg](const Ticker& ticker, const std::string& strategy, bool enabled) {
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

                    // T1-CALIB: Fetch per-ticker orderbook settings from MetaScalp.
                    // LargeAmountUsd calibrates density_min_size_usd dynamically.
                    if (ob_settings_loader_) {
                        auto obs = ob_settings_loader_->fetch(ticker);
                        if (obs) {
                            universe_.update_orderbook_settings(*obs);
                            LOG_INFO("[Universe] {} orderbook settings: large_amount_usd={}",
                                     ticker, obs->large_amount_usd);
                        }
                    }

                    // Ensure leader is also subscribed if it's not the current ticker
                    if (leader_name != ticker && !controllers_.contains(leader_name)) {
                        controllers_[leader_name] = std::make_unique<TickerController>(leader_name, *signal_bus_, universe_, *cluster_mgr_);
                        books_for_executor_[leader_name] = controllers_[leader_name]->book.get();
                        feed_->add_listener(leader_name, controllers_[leader_name].get());
                        feed_->subscribe_ticker(leader_name);
                    }
                }

                auto meta = universe_.meta(ticker).value_or(TickerMeta{0.01, 1e-6, 0.0, 0.0});
                TickerInfo info{ticker, "", "", true, meta.price_increment, meta.size_increment, meta.min_size, meta.max_size};

                if (strategy == "bounce") strategy_engine_->add_strategy(std::make_unique<BounceFromDensity>(ticker, info, bounce_cfg));
                else if (strategy == "breakout") strategy_engine_->add_strategy(std::make_unique<BreakoutEatThrough>(ticker, info, breakout_cfg));
                else if (strategy == "leaderlag") strategy_engine_->add_strategy(std::make_unique<LeaderLag>(ticker, info, leaderlag_cfg));
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

        ws->connect("ws://127.0.0.1:" + std::to_string(*port) + "/");
        feed_->start();

        // -- Universe discovery -------------------------------------------------
        auto allow_pats = Config::get_or<std::vector<std::string>>(
            "universe.pool.allow_patterns", std::vector<std::string>{"*USDT"});
        auto deny_pats  = Config::get_or<std::vector<std::string>>(
            "universe.pool.deny_patterns",
            std::vector<std::string>{"*UP*", "*DOWN*", "*BULL*", "*BEAR*", "*1000*"});
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
            LOG_WARN("[Universe] get_tickers failed ({}), pool will rely on screener only", e.what());
        }

        TickerUniverse::Config u_cfg;
        u_cfg.filters.allow_patterns = allow_pats;
        u_cfg.filters.deny_patterns  = deny_pats;
        u_cfg.max_pool_size = static_cast<std::size_t>(
            Config::get_or<int64_t>("universe.pool.max_pool_size", 30));
        u_cfg.exchange_volume_multiplier = exchange_volume_mult;
        // Dynamic per-coin threshold scaling.
        u_cfg.dynamic_thresholds_enabled =
            Config::get_or<bool>("universe.dynamic_thresholds.enabled", true);
        u_cfg.dynamic_thresholds_reference_volume =
            Config::get_or<double>("universe.dynamic_thresholds.reference_volume_24h_usd", 500'000'000.0);
        u_cfg.dynamic_thresholds_min_scale =
            Config::get_or<double>("universe.dynamic_thresholds.min_scale_factor", 0.15);
        u_cfg.dynamic_thresholds_max_scale =
            Config::get_or<double>("universe.dynamic_thresholds.max_scale_factor", 5.0);
        universe_.update_config(u_cfg);

        // Seed full candidate list so on_screener_new_coin() can build the pool.
        universe_.seed_candidates(all_candidates);
        pending_candidates_ = all_candidates;

        // No initial manual_allow pool — the screener (NotificationFeed) is the
        // sole authority on which coins to trade. Pool fills on first
        // notification_snapshot from MetaScalp (arrives within seconds).
        LOG_INFO("[Universe] Waiting for screener to populate pool ({} static-filtered candidates)",
                 all_candidates.size());
        // Don't call refresh_pool() — let the screener notification_snapshot drive it.

        // -- Notification feed (screener + BigTick) ----------------------------
        // Uses a separate WS connection so its on_message handler does not
        // conflict with MarketDataFeed's handler on the same socket.
        auto notif_ws = std::make_shared<BeastWsClient>(ioc_);
        notif_feed_ = std::make_unique<NotificationFeed>(notif_ws, universe_,
                                                         NotificationFeed::Config{});
        notif_ws->connect("ws://127.0.0.1:" + std::to_string(*port) + "/notifications");
        notif_feed_->start();
        LOG_INFO("[Universe] NotificationFeed started — screener will populate pool");

        // Fallback: if screener doesn't send ScreenerNewCoin within 10s,
        // activate the entire static-filtered candidate list as the pool.
        // This prevents the "empty dashboard" bug when MetaScalp screener
        // is not configured or not emitting ScreenerNewCoin events.
        pool_fallback_timer_.expires_after(std::chrono::seconds(10));
        pool_fallback_timer_.async_wait([this](const boost::system::error_code& ec) {
            if (ec) return;
            if (!universe_.active().empty()) return;
            if (pending_candidates_.empty()) {
                LOG_WARN("[Universe] Pool fallback triggered but no candidates available");
                return;
            }
            LOG_INFO("[Universe] Screener notifications not received within 10s — "
                     "activating {} candidates as fallback pool", pending_candidates_.size());
            universe_.refresh_pool(pending_candidates_);
            universe_.refresh_affinity();
        });
    }

    void schedule_tick() {
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

    void handle_kill_switch_() {
        LOG_CRITICAL("Kill-switch triggered — running emergency shutdown sequence");

        // Step 1: Cancel all pending orders for all active tickers
        for (const auto& [ticker, _] : controllers_) {
            try {
                executor_->cancel_all(ticker);
                LOG_INFO("[KillSwitch] Cancelled orders for {}", ticker);
            } catch (const std::exception& e) {
                LOG_ERROR("[KillSwitch] Failed to cancel orders for {}: {}", ticker, e.what());
            }
        }

        // Step 2: Close all open positions for all active tickers (market close)
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

        // Step 3: Persist final state (best-effort — market close orders are
        // async; actual fills will arrive via WS and update the account state
        // on the next run)
        if (persister_) {
            std::vector<ActiveTrade> active_snapshot;
            for (const auto& t : executor_->get_active_trades()) {
                if (t.state != TradeState::Closed) {
                    active_snapshot.push_back(t);
                }
            }
            persister_->save({account_state_, active_snapshot, last_reset_day_, true, "KillSwitch"});
            LOG_INFO("[KillSwitch] Final state persisted");
        }

        // Step 5: Exit with code 42
        LOG_CRITICAL("Kill-switch shutdown complete — exiting with code 42");
        std::_Exit(42);
    }

    void tick() {
        auto now = std::chrono::system_clock::now();
        
        // T4-RISK: Update AccountState from exchange via FinresHandler.
        // In paper mode the executor manages its own simulated balance — skip
        // finres so the real exchange balance (which may be 0 for a paper account)
        // does not overwrite the paper starting equity and trip the starting_equity
        // > 0 guard in RiskManager, blocking all plan evaluation.
        if (live_executor_ && finres_handler_) {
            auto finres = finres_handler_->get_snapshot(m_connection_id_for_tick);
            if (finres && finres->equity > 0.0) {
                account_state_.free_balance_usd = finres->balance;
                account_state_.equity_usd       = finres->equity;
            }
        }

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

            // Track consecutive losses for dashboard risk snapshot
            if (ct.pnl_usd < 0) {
                cached_consecutive_losses_++;
            } else {
                cached_consecutive_losses_ = 0;
            }

            TradeJournal::Entry je;
            je.plan          = ct.plan;
            je.pnl_usd       = ct.pnl_usd;
            je.exit_price    = ct.exit_price;
            je.cause_of_exit = ct.reason;
            je.ts_unix_ms    = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
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
            std::vector<ActiveTrade> active_snapshot;
            for (const auto& t : active) {
                if (t.state != TradeState::Closed) {
                    active_snapshot.push_back(t);
                }
            }
            persister_->save({account_state_, active_snapshot, last_reset_day_, false, ""});
            last_persist_ = now;
        }

        if (events_occurred || (now - last_dash_update_ > std::chrono::milliseconds(100))) {
            const std::size_t sc = dashboard_->session_count();
            if (sc > 0) {
                const bool slow_tick = events_occurred ||
                    (now - last_slow_dash_update_ > std::chrono::seconds(1));
                
                if (slow_tick) {
                    LOG_INFO("Dashboard update: journal={}, signals={}, universe={}, sessions={}", 
                             journal_.size(), recent_signals_.size(), universe_.active().size(), sc);
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

                    // Build per-strategy stats from recent journal entries
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

                    // Build funding info for active tickers
                    cached_funding_info_.clear();
                    for (const auto& ticker : universe_.active()) {
                        auto fi = feed_->get_funding(ticker);
                        if (fi) {
                            DashboardServer::State::FundingInfo info;
                            info.ticker = ticker;
                            info.rate = fi->rate;
                            info.next_funding_unix = static_cast<int64_t>(std::chrono::duration_cast<std::chrono::seconds>(fi->next_funding_time.time_since_epoch()).count());
                            cached_funding_info_.push_back(std::move(info));
                        }
                    }

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
                dash_state.metascalp.connected = feed_->is_connected();
                dash_state.metascalp.connection_name = "MetaScalp-Local";
                // Reuse all_marks from above — no second lock
                dash_state.universe_rows = cached_universe_;
                for (auto& row : dash_state.universe_rows) {
                    auto mit = all_marks.find(row.ticker);
                    if (mit != all_marks.end()) row.mark_price = mit->second;
                }
                dash_state.strategy_stats = cached_strategy_stats_;
                dash_state.funding_info = cached_funding_info_;
                dash_state.recorder_active = dump_recorder_.is_active();
                dash_state.recorder_path = dump_recorder_.path();
                dash_state.server_time_unix = std::chrono::duration_cast<std::chrono::seconds>(
                    now.time_since_epoch()).count();

                // Risk snapshot
                dash_state.risk.total_trades_today = static_cast<int>(cached_journal_.size());
                dash_state.risk.margin_used_pct = account_state_.equity_usd > 0
                    ? ((account_state_.equity_usd - account_state_.free_balance_usd) / account_state_.equity_usd) * 100.0 : 0.0;
                dash_state.risk.exposure_pct = account_state_.equity_usd > 0 && !active.empty()
                    ? (upnl_sum / account_state_.equity_usd) * 100.0 : 0.0;
                dash_state.risk.daily_pnl_pct = account_state_.starting_equity_usd > 0
                    ? (account_state_.realized_pnl_today_usd / account_state_.starting_equity_usd) * 100.0 : 0.0;
                dash_state.risk.current_drawdown_pct = account_state_.starting_equity_usd > 0
                    ? ((account_state_.equity_usd - account_state_.starting_equity_usd) / account_state_.starting_equity_usd) * 100.0
                    : 0.0;
                dash_state.risk.consecutive_losses = cached_consecutive_losses_;

                // DS-10: Collect dashboard state — strategy states, chart history, order book
                dash_state.strategy_states = strategy_engine_->get_all_states();

                // DS-23: For the selected ticker (or leader as default), collect chart & OB data
                std::string sel_ticker = dashboard_->get_selected_ticker();
                if (sel_ticker.empty()) sel_ticker = leader_ticker;
                dash_state.selected_ticker = sel_ticker;

                if (auto it_ctrl = controllers_.find(sel_ticker); it_ctrl != controllers_.end()) {
                    dash_state.chart_history = it_ctrl->second->chart_snapshot();
                    auto [bids, asks] = it_ctrl->second->ob_snapshot(20);
                    dash_state.bids_top20 = std::move(bids);
                    dash_state.asks_top20 = std::move(asks);

                    // Compute mid, spread, imbalance from top-of-book
                    if (it_ctrl->second->book) {
                        auto best_bid = it_ctrl->second->book->best_bid();
                        auto best_ask = it_ctrl->second->book->best_ask();
                        if (best_bid && best_ask) {
                            dash_state.ob_mid = 0.5 * (*best_bid + *best_ask);
                            dash_state.ob_spread_bps = (*best_ask - *best_bid) / *best_bid * kBpsBase;
                            double bid_d = it_ctrl->second->book->bid_depth(10);
                            double ask_d = it_ctrl->second->book->ask_depth(10);
                            double total = bid_d + ask_d;
                            dash_state.ob_imbalance = (total > 0) ? (bid_d - ask_d) / total : 0.0;
                        }
                    }
                }

                dashboard_->update_state_async(std::move(dash_state));
            }
            // Bug #3 fix: only update last_dash_update_ when we actually sent data.
            // Prevents slow_tick from being suppressed when browser reconnects.
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
    boost::asio::steady_timer pool_fallback_timer_;

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
    std::vector<DashboardServer::State::StrategyStats> cached_strategy_stats_;
    std::vector<DashboardServer::State::FundingInfo> cached_funding_info_;
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
    std::shared_ptr<CurlHttpClient> http_client_;
    std::unique_ptr<FinresHandler> finres_handler_;
    std::unique_ptr<OrderbookSettingsLoader> ob_settings_loader_;

    std::map<Ticker, std::unique_ptr<TickerController>> controllers_;
    std::map<Ticker, const OrderBook*> books_for_executor_;
    std::map<Ticker, std::chrono::system_clock::time_point> last_funding_times_;
    int m_connection_id_for_tick{1};
    bool live_executor_{false};  // true only when executor.mode == "live"
    std::vector<Ticker> pending_candidates_;  // fallback pool when screener doesn't respond
    std::map<std::string, int> signal_counts_;
    std::deque<DashboardServer::State::SignalEvent> recent_signals_; // newest first, max 60
    TradeJournal journal_{"journal"};
    int cached_consecutive_losses_{0};
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

