#include "app/BotApp.hpp"
#include "app/PipelineProcessor.hpp"
#include <unordered_map>

#include "control/ClockDriftMonitor.hpp"
#include "control/SntpClient.hpp"
#include "control/TickerController.hpp"
#include "executor/LiveExecutor.hpp"
#include "executor/PaperExecutor.hpp"
#include "executor/StartupRecovery.hpp"
#include "logger/Logger.hpp"
#include "logger/TradeJournal.hpp"
#include "marketdata/ClusterSnapshot.hpp"
#include "metrics/AlertWebhook.hpp"
#include "metrics/MetricsExporter.hpp"
#include "metrics/MetricsRegistry.hpp"
#include "perf/LatencyTracer.hpp"
#include "perf/PerfRegistry.hpp"
#include "risk/AccountStatePersister.hpp"
#include "risk/TradingDay.hpp"
#include "signals/SignalLevelBridge.hpp"
#include "strategy/BounceFromDensity.hpp"
#include "strategy/BreakoutEatThrough.hpp"
#include "strategy/FlushReversal.hpp"
#include "strategy/LeaderLag.hpp"
#include "strategy/StrategyEngine.hpp"
#include "transport/BeastWsClient.hpp"
#include "transport/ClusterSnapshotClient.hpp"
#include "transport/CurlHttpClient.hpp"
#include "transport/FinresHandler.hpp"
#include "transport/MarketDataFeed.hpp"
#include "transport/MetaScalpDiscovery.hpp"
#include "transport/NotificationFeed.hpp"
#include "transport/OrderGateway.hpp"
#include "transport/SignalLevelGateway.hpp"
#include "transport/external/ExternalIoContext.hpp"
#include "transport/external/ExternalFeedRegistry.hpp"
#include "universe/OrderbookSettingsLoader.hpp"
#include "universe/UniverseFilters.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <memory>
#include <optional>
#include <pthread.h>
#include <signal.h>
#include <sstream>
#include <string_view>
#include <thread>
#include <spdlog/fmt/ranges.h>

namespace trade_bot {

// ═══════════════════════════════════════════════════════════════
// Constructor / Destructor
// ═══════════════════════════════════════════════════════════════

BotApp::BotApp(boost::asio::io_context& ioc)
    : ioc_(ioc)
    , timer_(ioc)
    , dashboard_timer_(ioc)
    , pool_fallback_timer_(ioc)
    , signal_level_cleanup_timer_(ioc)
    , last_reset_day_(TradingDay::current_date_utc())
    , last_persist_(std::chrono::system_clock::now())
    , last_dash_update_(std::chrono::system_clock::now())
    , last_slow_dash_update_(std::chrono::system_clock::time_point{})
    , kill_switch_(&KillSwitch::instance())
    , system_clock_(std::make_shared<WallClock>()) {}

BotApp::~BotApp() {
    // Dump performance report on normal shutdown
    dump_perf_report_();

    // Stop main ioc first so no async handlers fire against half-destroyed members.
    ioc_.stop();

    // Clear registry to destroy external clients and their Asio timers safely while ExternalIoContext is still running.
    ExternalFeedRegistry::instance().clear();
    ExternalIoContext::instance().stop();

    dashboard_ioc_.stop();
    if (dashboard_thread_.joinable()) {
        try {
            dashboard_thread_.join();
        } catch (const std::system_error& e) {
            LOG_WARN("dashboard_thread join error: {}", e.what());
        }
    }
}

void BotApp::run() {
    init_components();
    schedule_tick();
    schedule_dashboard_tick();
}

// ═══════════════════════════════════════════════════════════════
// Bootstrap
// ═══════════════════════════════════════════════════════════════

void BotApp::init_config_and_logger_() {
    if (std::filesystem::exists("./killswitch"))
        throw std::runtime_error("Kill-switch file present");
    if (!std::filesystem::exists("config.toml"))
        throw std::runtime_error("Config 'config.toml' not found");
    Config::load("config.toml");
    Logger::init(
        Config::get_or<std::string>("logger.path",  "logs/trade_bot.log"),
        Config::get_or<std::string>("logger.level", "info"));
    persister_ = std::make_unique<AccountStatePersister>("journal/account_state.json");
    LOG_INFO("trade_bot {} starting...", Config::get<std::string>("app.version"));
    kill_switch_ = &KillSwitch::instance();
    kill_switch_->start();
}

void BotApp::register_strategy_affinities_(std::shared_ptr<double> exchange_mult) {
    // GAP-4 FIX: each lambda now gates on funding_rate_bps and volatility_1min_bps
    // in addition to volume and spread. Values come from ticker_stats_cache_ which is
    // updated every tick from live order books and funding data.

    double bounce_min_vol      = Config::get_or<double>("universe.affinity.bounce.min_volume_24h_usd",      100000000.0);
    double bounce_max_spread   = Config::get_or<double>("universe.affinity.bounce.max_avg_spread_bps",      3.0);
    double bounce_max_funding  = Config::get_or<double>("universe.affinity.bounce.max_funding_rate_bps",    2.0);
    double bounce_max_vol_1min = Config::get_or<double>("universe.affinity.bounce.max_volatility_1min_bps", 30.0);
    universe_.register_strategy("bounce",
        [this, bounce_min_vol, bounce_max_spread, bounce_max_funding, bounce_max_vol_1min, exchange_mult]
        (const Ticker& t) {
            auto stats = universe_.get_stats(t);
            if (!stats) return true;
            if (stats->volume_24h_usd > 0.0 &&
                stats->volume_24h_usd < bounce_min_vol * (*exchange_mult)) return false;
            if (stats->avg_spread_bps > 0.0 &&
                stats->avg_spread_bps > bounce_max_spread) return false;
            if (std::abs(stats->funding_rate_bps) > bounce_max_funding) return false;
            if (stats->volatility_1min_bps > 0.0 &&
                stats->volatility_1min_bps > bounce_max_vol_1min) return false;
            return true;
    });

    double breakout_min_vol     = Config::get_or<double>("universe.affinity.breakout.min_volume_24h_usd",   100000000.0);
    double breakout_max_spread  = Config::get_or<double>("universe.affinity.breakout.max_avg_spread_bps",   4.0);
    double breakout_max_funding = Config::get_or<double>("universe.affinity.breakout.max_funding_rate_bps", 5.0);
    universe_.register_strategy("breakout",
        [this, breakout_min_vol, breakout_max_spread, breakout_max_funding, exchange_mult]
        (const Ticker& t) {
            auto stats = universe_.get_stats(t);
            if (!stats) return true;
            if (stats->volume_24h_usd > 0.0 &&
                stats->volume_24h_usd < breakout_min_vol * (*exchange_mult)) return false;
            if (stats->avg_spread_bps > 0.0 &&
                stats->avg_spread_bps > breakout_max_spread) return false;
            if (std::abs(stats->funding_rate_bps) > breakout_max_funding) return false;
            return true;
    });

    double leaderlag_min_vol     = Config::get_or<double>("universe.affinity.leaderlag.min_volume_24h_usd",      50000000.0);
    double leaderlag_max_spread  = Config::get_or<double>("universe.affinity.leaderlag.max_avg_spread_bps",      3.0);
    double leaderlag_max_funding = Config::get_or<double>("universe.affinity.leaderlag.max_funding_rate_bps",    3.0);
    double leaderlag_min_vol_1min = Config::get_or<double>("universe.affinity.leaderlag.min_volatility_1min_bps", 3.0);
    universe_.register_strategy("leaderlag",
        [this, leaderlag_min_vol, leaderlag_max_spread, leaderlag_max_funding, leaderlag_min_vol_1min, exchange_mult]
        (const Ticker& t) {
            auto stats = universe_.get_stats(t);
            if (!stats) return true;
            if (stats->volume_24h_usd > 0.0 &&
                stats->volume_24h_usd < leaderlag_min_vol * (*exchange_mult)) return false;
            if (stats->avg_spread_bps > 0.0 &&
                stats->avg_spread_bps > leaderlag_max_spread) return false;
            if (std::abs(stats->funding_rate_bps) > leaderlag_max_funding) return false;
            // LeaderLag needs momentum: too little volatility = no lag signal
            if (stats->volatility_1min_bps > 0.0 &&
                stats->volatility_1min_bps < leaderlag_min_vol_1min) return false;
            return true;
    });

    auto get_flush_affinity = [](std::string_view key, double def) {
        const std::string modern = "universe.affinity.flushreversal." + std::string(key);
        const std::string legacy = "universe.affinity.flush." + std::string(key);
        return Config::has(modern) ? Config::get_or<double>(modern, def)
                                   : Config::get_or<double>(legacy, def);
    };
    double flush_min_vol     = get_flush_affinity("min_volume_24h_usd",   50000000.0);
    double flush_max_spread  = get_flush_affinity("max_avg_spread_bps",   4.0);
    double flush_max_funding = get_flush_affinity("max_funding_rate_bps", 4.0);
    const bool flush_live_enabled = Config::get_or<bool>("strategies.flushreversal.allow_live", false);
    universe_.register_strategy("flushreversal",
        [this, flush_min_vol, flush_max_spread, flush_max_funding, flush_live_enabled, exchange_mult]
        (const Ticker& t) {
            if (live_executor_ && !flush_live_enabled) return false;
            auto stats = universe_.get_stats(t);
            if (!stats) return true;
            if (stats->volume_24h_usd > 0.0 &&
                stats->volume_24h_usd < flush_min_vol * (*exchange_mult)) return false;
            if (stats->avg_spread_bps > 0.0 &&
                stats->avg_spread_bps > flush_max_spread) return false;
            // Extreme funding = very crowded trade, flush reversals in crowded markets fail often
            if (std::abs(stats->funding_rate_bps) > flush_max_funding) return false;
            return true;
    });
}

void BotApp::start_dashboard_thread_() {
    const auto addr  = Config::get_or<std::string>("dashboard.bind_address", "127.0.0.1");
    const auto port  = static_cast<uint16_t>(Config::get_or<int64_t>("dashboard.port", 8080));
    const auto token = Config::get_or<std::string>("dashboard.auth_token", std::string{});
    dashboard_ = std::make_unique<DashboardServer>(dashboard_ioc_, addr, port, token);

    dashboard_->set_command_handler([this](const std::string& cmd) -> nlohmann::json {
        LOG_INFO("[Dashboard] Received command: {}", cmd);
        std::istringstream iss(cmd);
        std::string token;
        iss >> token;
        if (token == "order") {
            // order BUY 0.1 50000 BTCUSDT
            std::string side_str;
            double size = 0.0, price = 0.0;
            std::string ticker;
            if (iss >> side_str >> size >> price >> ticker) {
                trade_bot::TradePlan plan;
                plan.ticker = ticker;
                plan.side = (side_str == "BUY" || side_str == "LONG") ? Side::Buy : Side::Sell;
                plan.size_coin = size;
                plan.entry_price = price;
                plan.strategy_name = "Manual";
                executor_->submit(plan);
                return {{"ok", true}, {"msg", "Order submitted"}};
            }
            return {{"ok", false}, {"error", "Invalid order format"}};
        } else if (token == "strategy") {
            std::string action, ticker, class_name;
            if (iss >> action >> ticker) {
                iss >> class_name; // optional: specific strategy class name
                // Reverse-map class name → short affinity key
                auto to_short = [](const std::string& cls) -> std::string {
                    if (cls == "BounceFromDensity")  return "bounce";
                    if (cls == "BreakoutEatThrough") return "breakout";
                    if (cls == "LeaderLag")          return "leaderlag";
                    if (cls == "FlushReversal")      return "flushreversal";
                    return cls; // pass through if already short name or unknown
                };
                bool enabled = (action == "enable");
                if (!class_name.empty()) {
                    universe_.override_affinity(ticker, to_short(class_name), enabled);
                } else {
                    for (const char* s : {"bounce","breakout","leaderlag","flushreversal"})
                        universe_.override_affinity(ticker, s, enabled);
                }
                return {{"ok", true}};
            }
            return {{"ok", false}, {"error", "Invalid strategy command"}};
        } else if (token == "close") {
            std::string ticker;
            if (iss >> ticker) {
                executor_->close_trade(ticker, "Manual");
                return {{"ok", true}, {"msg", "Close requested"}};
            }
            return {{"ok", false}, {"error", "Missing ticker"}};
        }
        return {{"ok", false}, {"error", "Unknown command"}};
    });

    dashboard_->start();

    const auto m_addr  = Config::get_or<std::string>("metrics.bind_address", "127.0.0.1");
    const auto m_port  = static_cast<uint16_t>(Config::get_or<int64_t>("metrics.port", 9090));
    const auto m_token = Config::get_or<std::string>("metrics.auth_token", std::string{});
    metrics_exporter_ = std::make_unique<MetricsExporter>(dashboard_ioc_, m_addr, m_port, m_token);
    metrics_exporter_->start();

    dashboard_thread_ = std::thread([this]() {
        // M2: Block SIGPIPE in worker threads; only main thread handles signals.
        sigset_t mask;
        sigemptyset(&mask);
        sigaddset(&mask, SIGPIPE);
        pthread_sigmask(SIG_BLOCK, &mask, nullptr);
        auto work = boost::asio::make_work_guard(dashboard_ioc_);
        dashboard_ioc_.run();
    });
}

void BotApp::subscribe_signal_bus_() {
    signal_bus_ = std::make_unique<SignalBus>();

    // Per-(ticker,kind) cooldown for the dashboard ring buffer.
    // Prevents high-frequency detectors (LevelApproach, LeaderMove) from
    // flooding the panel. Actual signal bus is NOT throttled — strategies
    // receive every signal. Only the dashboard display is deduplicated.
    using Clock = std::chrono::system_clock;
    static constexpr int kDashCooloffMs = 3000;
    std::unordered_map<std::string, Clock::time_point> dash_cooloffs;

    signal_bus_->subscribe([&, dash_cooloffs = std::move(dash_cooloffs)](const Signal& s) mutable {
        MetricsRegistry::instance().counter_inc(
            "trade_bot_signals_total",
            {{"kind", std::to_string(static_cast<int>(s.kind))}});
        static constexpr const char* kKindNames[] = {
            "DensityDetected","DensityRemoved","DensityEating",
            "IcebergSuspected","TapeBurst","TapeFade","TapeFlush","TapeDistribution",
            "LevelFormed","LevelApproach","LevelRejection","LevelBreak","LeaderMove"};
        const auto idx = static_cast<std::size_t>(s.kind);
        const char* kind_name = idx < std::size(kKindNames) ? kKindNames[idx] : "Unknown";
        signal_counts_[kind_name]++;

        // Dashboard cooldown: skip if same (ticker, kind) was added within kDashCooloffMs.
        std::string dash_key = s.ticker + "|" + kind_name;
        auto now = s.timestamp;
        auto it = dash_cooloffs.find(dash_key);
        bool in_cooloff = (it != dash_cooloffs.end()) &&
            (std::chrono::duration_cast<std::chrono::milliseconds>(now - it->second).count()
             < kDashCooloffMs);
        if (in_cooloff) return;

        // NEW-02 fix: prune stale entries as tickers rotate in/out of the universe.
        if (dash_cooloffs.size() > 500) {
            auto cutoff = now - std::chrono::milliseconds(kDashCooloffMs * 2);
            for (auto map_it = dash_cooloffs.begin(); map_it != dash_cooloffs.end(); ) {
                if (map_it->second < cutoff) map_it = dash_cooloffs.erase(map_it);
                else ++map_it;
            }
        }
        dash_cooloffs[dash_key] = now;

        // Ring buffer for dashboard feed — called on ioc_ thread, no mutex needed.
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
        ev.side       = std::string(s.payload.side);
        recent_signals_.push_front(std::move(ev));
        if (recent_signals_.size() > 60) recent_signals_.pop_back();

        // Iceberg sonar: push to DashboardServer sonar ring directly
        if (s.kind == SignalKind::IcebergSuspected && dashboard_) {
            auto ts_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                s.timestamp.time_since_epoch()).count();
            std::string ice_side = (std::string_view(s.payload.side) == "Bid") ? "BID" : "ASK";
            dashboard_->push_iceberg_event(ts_ms, s.price, std::move(ice_side),
                                           s.payload.total_eaten_usd);
        }
    });
}

RiskManager::Config BotApp::load_risk_config_() {
    RiskManager::Config rm;
    rm.max_daily_loss_pct        = Config::get_or<double> ("risk.max_daily_loss_pct",        3.0);
    rm.max_concurrent_positions  = static_cast<int>(Config::get_or<int64_t>("risk.max_concurrent_positions",  3));
    rm.max_per_trade_risk_pct    = Config::get_or<double> ("risk.max_per_trade_risk_pct",    0.5);
    rm.max_position_value_pct    = Config::get_or<double> ("risk.max_position_value_pct",   10.0);
    rm.max_leverage              = static_cast<int>(Config::get_or<int64_t>("risk.max_leverage",              5));
    rm.news_blackout_min         = static_cast<int>(Config::get_or<int64_t>("risk.news_blackout_min",         5));
    rm.news_calendar_check_min   = static_cast<int>(Config::get_or<int64_t>("risk.news_calendar_check_min",  60));
    rm.news_calendar_require_fresh = Config::get_or<bool>("risk.news_calendar_require_fresh", false);
    rm.funding_blackout_pre_sec  = static_cast<int>(Config::get_or<int64_t>("risk.funding_blackout_pre_sec", 30));
    rm.funding_blackout_post_sec = static_cast<int>(Config::get_or<int64_t>("risk.funding_blackout_post_sec",30));
    rm.min_stop_bps              = Config::get_or<double> ("risk.min_stop_bps",              3.0);
    rm.max_stop_bps              = Config::get_or<double> ("risk.max_stop_bps",             20.0);
    rm.min_rr_ratio              = Config::get_or<double> ("risk.min_rr_ratio",              1.0);
    rm.max_positions_per_ticker  = static_cast<int>(Config::get_or<int64_t>("risk.max_positions_per_ticker",  1));
    return rm;
}

void BotApp::load_account_state_() {
    auto persisted = persister_->load();
    if (persisted) {
        account_state_ = persisted->account_state;
        last_reset_day_ = persisted->last_reset_day_utc;
        // Guard: recover from a zeroed equity persisted by a prior bug.
        if (account_state_.equity_usd <= 0.0 && account_state_.starting_equity_usd > 0.0) {
            account_state_.equity_usd       = account_state_.starting_equity_usd;
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
    
    // P0 DETERMINISM: Initialize Kahan accumulators with loaded state
    realized_pnl_accumulator_.add(account_state_.realized_pnl_today_usd);
    equity_accumulator_.add(account_state_.equity_usd);
    free_balance_accumulator_.add(account_state_.free_balance_usd);
    account_state_.trading_day_start = std::chrono::system_clock::now();
}

void BotApp::maybe_load_news_() {
    if (!std::filesystem::exists("config/news.json")) return;
    try {
        news_.load("config/news.json");
        news_.start_watching();
    } catch (const std::exception& e) {
        LOG_WARN("Failed to load news calendar: {}", e.what());
    }
}

BotApp::ConnectionInfo BotApp::discover_connection_(std::shared_ptr<OrderGateway> gateway,
                                                    std::optional<double> cfg_mult) {
    ConnectionInfo info;
    std::string connection_name = "Unknown";
    try {
        auto connections = gateway->get_connections();
        for (const auto& conn : connections) {
            if (conn.name.find("Futures") != std::string::npos) {
                info.id   = conn.id;
                connection_name = conn.name;
                LOG_INFO("Selected connection {}: {}", info.id, conn.name);
                break;
            }
        }
        if (cfg_mult) {
            info.exchange_volume_mult = *cfg_mult;
            LOG_INFO("[Universe] Exchange volume multiplier overridden by config: {:.3f}x",
                     info.exchange_volume_mult);
        } else {
            std::string name_lower = connection_name;
            std::transform(name_lower.begin(), name_lower.end(), name_lower.begin(), ::tolower);
            if (name_lower.find("mexc") != std::string::npos) {
                info.exchange_volume_mult = 0.1;
                LOG_INFO("[Universe] MEXC detected — volume thresholds scaled by 0.1x");
            } else if (name_lower.find("bybit") != std::string::npos) {
                info.exchange_volume_mult = 0.5;
                LOG_INFO("[Universe] Bybit detected — volume thresholds scaled by 0.5x");
            }
        }
    } catch (const std::exception& e) {
        LOG_WARN("Failed to get connections, using default ID 1: {}", e.what());
    }
    return info;
}

BotApp::StrategyConfigs BotApp::load_strategy_configs_() {
    StrategyConfigs c;
    c.bounce.entry_offset_bps = Config::get_or<double>("strategies.bounce.entry_offset_bps", 3.0);
    c.bounce.stop_buffer_bps  = Config::get_or<double>("strategies.bounce.stop_buffer_bps",  5.0);
    c.bounce.max_spread_bps   = Config::get_or<double>("strategies.bounce.max_spread_bps",   3.0);
    c.bounce.tp1_size_ratio   = Config::get_or<double>("strategies.bounce.tp1_size_ratio",   0.5);
    c.bounce.burst_contra_exit_sec = std::chrono::seconds(
        Config::get_or<int64_t>("strategies.bounce.burst_contra_exit_sec", 5));
    c.bounce.min_density_age_ms = std::chrono::milliseconds(
        Config::get_or<int64_t>("strategies.bounce.min_density_age_ms", 5000));
    c.bounce.no_progress_timeout_sec = Config::get_or<double>("strategies.bounce.no_progress_timeout_sec", 120.0);

    c.breakout.aggressive_offset_bps       = Config::get_or<double>("strategies.breakout.aggressive_offset_bps",       2.0);
    c.breakout.stop_buffer_bps             = Config::get_or<double>("strategies.breakout.stop_buffer_bps",             5.0);
    c.breakout.tp1_size_ratio              = Config::get_or<double>("strategies.breakout.tp1_size_ratio",              0.5);
    c.breakout.min_tape_aggression         = Config::get_or<double>("strategies.breakout.min_tape_aggression",         0.3);
    c.breakout.min_relative_volume         = Config::get_or<double>("strategies.breakout.min_relative_volume",         1.5);
    c.breakout.max_resistance_cluster_ratio = Config::get_or<double>("strategies.breakout.max_resistance_cluster_ratio", 0.7);
    c.breakout.fade_contra_exit_sec   = std::chrono::seconds(Config::get_or<int64_t>("strategies.breakout.fade_contra_exit_sec",   5));
    c.breakout.leader_contra_exit_sec = std::chrono::seconds(Config::get_or<int64_t>("strategies.breakout.leader_contra_exit_sec", 5));
    c.breakout.leader_exit_contra_pct = Config::get_or<double>("strategies.breakout.leader_exit_contra_pct", 0.15);
    c.breakout.no_progress_timeout_sec = Config::get_or<double>("strategies.breakout.no_progress_timeout_sec", 60.0);
    c.breakout.post_entry_grace_sec    = Config::get_or<double>("strategies.breakout.post_entry_grace_sec",    5.0);
    c.breakout.min_follow_through_bps  = Config::get_or<double>("strategies.breakout.min_follow_through_bps",  10.0);

    c.leaderlag.stop_distance_bps          = Config::get_or<double>("strategies.leaderlag.stop_distance_bps",          8.0);
    c.leaderlag.tp1_size_ratio             = Config::get_or<double>("strategies.leaderlag.tp1_size_ratio",             0.6);
    c.leaderlag.correlation_exit_threshold = Config::get_or<double>("strategies.leaderlag.correlation_exit_threshold", 0.3);
    c.leaderlag.leader_exit_reversal_bps   = Config::get_or<double>("strategies.leaderlag.leader_exit_reversal_bps",   5.0);
    c.leaderlag.swing_lookback_ticks      = static_cast<int>(
        Config::get_or<int64_t>("strategies.leaderlag.swing_lookback_ticks", 30));
    c.leaderlag.no_progress_timeout_sec   = Config::get_or<double>("strategies.leaderlag.no_progress_timeout_sec", 15.0);

    auto flush_key = [](std::string_view key) {
        return "strategies.flushreversal." + std::string(key);
    };
    auto legacy_flush_key = [](std::string_view key) {
        return "strategies.flush." + std::string(key);
    };
    auto get_flush_double = [&](std::string_view key, double def) {
        const auto modern = flush_key(key);
        const auto legacy = legacy_flush_key(key);
        return Config::has(modern) ? Config::get_or<double>(modern, def)
                                   : Config::get_or<double>(legacy, def);
    };
    auto get_flush_i64 = [&](std::string_view key, int64_t def) {
        const auto modern = flush_key(key);
        const auto legacy = legacy_flush_key(key);
        return Config::has(modern) ? Config::get_or<int64_t>(modern, def)
                                   : Config::get_or<int64_t>(legacy, def);
    };
    c.flushreversal.entry_offset_bps        = get_flush_double("entry_offset_bps",        2.0);
    c.flushreversal.stop_buffer_bps         = get_flush_double("stop_buffer_bps",         8.0);
    c.flushreversal.tp1_r                   = get_flush_double("tp1_r",                   2.0);
    c.flushreversal.tp1_size_ratio          = get_flush_double("tp1_size_ratio",          0.6);
    c.flushreversal.max_spread_bps          = get_flush_double("max_spread_bps",          4.0);
    c.flushreversal.min_flush_dist_bps      = get_flush_double("min_flush_dist_bps",      3.0);
    c.flushreversal.max_vol_fade_ratio      = get_flush_double("max_vol_fade_ratio",      0.5);
    c.flushreversal.min_price_reversal_bps  = get_flush_double("min_price_reversal_bps",  1.5);
    c.flushreversal.no_progress_timeout_sec = get_flush_double("no_progress_timeout_sec", 30.0);
    c.flushreversal.min_flush_count = static_cast<int>(
        get_flush_i64("min_flush_count", 2));
    c.flushreversal.flush_count_window = std::chrono::seconds(
        get_flush_i64("flush_window_sec", 120));
    c.flushreversal.entry_timeout = std::chrono::seconds(
        get_flush_i64("entry_timeout_sec", 8));
    c.flushreversal.min_level_touches = static_cast<int>(
        get_flush_i64("min_level_touches", 2));
    return c;
}

// ═══════════════════════════════════════════════════════════════
// Wiring
// ═══════════════════════════════════════════════════════════════

void BotApp::init_feed_and_executor_(std::shared_ptr<BeastWsClient> ws, int port,
                                      int connection_id,
                                      std::shared_ptr<OrderGateway> gateway) {
    finres_handler_ = std::make_unique<FinresHandler>(system_clock_);
    feed_ = std::make_unique<MarketDataFeed>(ws, connection_id);
    feed_->set_orderbook_snapshot_fetcher(
        [gateway, connection_id](const Ticker& ticker,
                                 int zoom_index,
                                 std::optional<int> depth_levels,
                                 std::optional<double> depth_percent) {
            return gateway->get_orderbook_snapshot(
                connection_id, ticker, zoom_index, depth_levels, depth_percent);
        });
    feed_->add_listener(finres_handler_.get());
    m_connection_id_for_tick = connection_id;
    feed_->set_record_tap([this](const nlohmann::json& msg, int64_t ts_ns) {
        dump_recorder_.record(msg, ts_ns);
    });
    dashboard_->set_recorder(&dump_recorder_);
    cluster_client_ = std::make_unique<ClusterSnapshotClient>(
        *http_client_, "http://localhost:" + std::to_string(port), connection_id);
    cluster_mgr_ = std::make_unique<ClusterSnapshotManager>(*cluster_client_);
    cluster_mgr_->start();
    ob_settings_loader_ = std::make_unique<OrderbookSettingsLoader>(
        *http_client_, "http://localhost:" + std::to_string(port), connection_id);

    const std::string mode = Config::get_or<std::string>("executor.mode", "paper");
    live_executor_ = (mode == "live");
    gateway_ = gateway;
    if (live_executor_) {
        executor_ = std::make_unique<LiveExecutor>(connection_id, *gateway, *feed_, universe_);
    } else {
        executor_ = std::make_unique<PaperExecutor>(books_for_executor_);
    }
    feed_->add_listener(executor_.get());

    StartupRecovery recovery(connection_id, *gateway, *persister_);
    auto recovery_res = recovery.run();
    for (const auto& entry : recovery_res.log_entries) LOG_INFO("[Recovery] {}", entry);
    executor_->inject_recovered_trades(recovery_res.recovered_trades);
}

void BotApp::setup_strategy_engine_callbacks_() {
    strategy_engine_->set_on_plan([&](const TradePlan& plan) {
        // FEAT-06: §0.7 affinity_stable_min — ticker must be in pool for ≥5 min before first trade
        static constexpr std::chrono::seconds kAffinityStableMin{300};
        if (!universe_.is_affinity_stable(plan.ticker, kAffinityStableMin,
                                          system_clock_->now())) {
            LOG_INFO("[Risk] Plan REJECTED for {}: affinity too recent (need {}s stable)",
                     plan.ticker, kAffinityStableMin.count());
            strategy_engine_->reset_strategy_plan(plan.ticker, plan.strategy_name.c_str());
            return;
        }
        static auto& plan_to_risk_hist = PerfRegistry::instance().get_or_create(kStagePlanToRisk);
        static auto& risk_to_submit_hist = PerfRegistry::instance().get_or_create(kStageRiskToSubmit);
        
        auto decision = [&]() {
            LatencyTracer trace(plan_to_risk_hist);
            return risk_manager_->evaluate(plan, account_state_);
        }();
        if (decision.accepted) {
            TradePlan accepted_plan = plan;
            accepted_plan.size_coin = decision.adjusted_size_coin;
            accepted_plan.risk_usd  = decision.risk_usd;
            auto meta = universe_.meta(plan.ticker).value_or(TickerMeta{0.01, 1e-6, 0.0, 0.0});
            if (meta.price_increment > 0.0) {
                auto round_p = [&](double p) {
                    return std::round(p / meta.price_increment) * meta.price_increment;
                };
                accepted_plan.entry_price = round_p(accepted_plan.entry_price);
                accepted_plan.stop_price  = round_p(accepted_plan.stop_price);
                accepted_plan.tp1_price   = round_p(accepted_plan.tp1_price);
                if (accepted_plan.tp2_price)
                    accepted_plan.tp2_price = round_p(*accepted_plan.tp2_price);
            }
            LOG_INFO("[Strategy] SUBMITTING plan for {}: {} @ {}, size={}",
                     plan.ticker, plan.side == Side::Buy ? "BUY" : "SELL",
                     accepted_plan.entry_price, accepted_plan.size_coin);
            {
                LatencyTracer trace(risk_to_submit_hist);
                executor_->submit(accepted_plan);
            }
            strategy_engine_->notify_plan_accepted(accepted_plan);
        } else {
            LOG_INFO("[Risk] Plan REJECTED for {}: {} ({})",
                     plan.ticker, static_cast<int>(decision.reason), decision.details);
            strategy_engine_->reset_strategy_plan(plan.ticker, plan.strategy_name.c_str());
        }
    });
    strategy_engine_->set_close_callback([&](const Ticker& ticker, const FixedString<32>& reason) {
        LOG_INFO("[Strategy] CLOSE requested for {} — {}", ticker, reason);
        executor_->close_trade(ticker, reason);
    });
}

void BotApp::setup_affinity_handler_(BounceFromDensity::Config     bounce_cfg,
                                      BreakoutEatThrough::Config   breakout_cfg,
                                      LeaderLag::Config             leaderlag_cfg,
                                      FlushReversal::Config         flushreversal_cfg) {
    universe_.set_affinity_change_handler(
        [&, bounce_cfg, breakout_cfg, leaderlag_cfg, flushreversal_cfg](
            const Ticker& ticker, const std::string& strategy, bool enabled) {
        LOG_INFO("[Universe] {} {} for {}", enabled ? "ENABLED" : "DISABLED", strategy, ticker);
        if (enabled) {
            if (!controllers_.contains(ticker)) {
                std::string leader_name = normalize_ticker_(
                    Config::get_or<std::string>("universe.affinity.leaderlag.require_leader", "BTC_USDT"));
                controllers_[ticker] = std::make_unique<TickerController>(
                    ticker, *signal_bus_, universe_, *cluster_mgr_, leader_name);
                books_for_executor_[ticker] = controllers_[ticker]->book.get();
                feed_->add_listener(ticker, controllers_[ticker].get());
                feed_->subscribe_ticker(ticker);
                
                // Register ticker components in PipelineProcessor for event-driven processing
                if (processor_) {
                    auto& ctrl = controllers_[ticker];
                    processor_->register_ticker(
                        ticker,
                        ctrl->book.get(),
                        ctrl->stream.get(),
                        ctrl->leader_tracker.get(),
                        ctrl->extractor.get(),
                        ctrl.get()
                    );
                    LOG_INFO("[PipelineProcessor] Registered components for {}", ticker);
                }
                if (ob_settings_loader_) {
                    try {
                        auto obs = ob_settings_loader_->fetch(ticker);
                        if (obs) {
                            universe_.update_orderbook_settings(*obs);
                            LOG_INFO("[Universe] {} orderbook settings: large_amount_usd={}",
                                     ticker, obs->large_amount_usd);
                        }
                    } catch (const std::exception& e) {
                        LOG_WARN("[Universe] ob_settings fetch failed for {}: {}", ticker, e.what());
                    }
                }
                if (leader_name != ticker && !controllers_.contains(leader_name)) {
                    controllers_[leader_name] = std::make_unique<TickerController>(
                        leader_name, *signal_bus_, universe_, *cluster_mgr_);
                    books_for_executor_[leader_name] = controllers_[leader_name]->book.get();
                    feed_->add_listener(leader_name, controllers_[leader_name].get());
                    feed_->subscribe_ticker(leader_name);
                    if (processor_) {
                        auto& ctrl = controllers_[leader_name];
                        processor_->register_ticker(
                            leader_name,
                            ctrl->book.get(),
                            ctrl->stream.get(),
                            ctrl->leader_tracker.get(),
                            ctrl->extractor.get(),
                            ctrl.get()
                        );
                        LOG_INFO("[PipelineProcessor] Registered components for dynamic leader {}", leader_name);
                    }
                }
            }
            auto meta = universe_.meta(ticker).value_or(TickerMeta{0.01, 1e-6, 0.0, 0.0});
            TickerInfo info{ticker, "", "", true, meta.price_increment, meta.size_increment, meta.min_size, meta.max_size};

            // Per-ticker scaling: sqrt(sf) smooths extremes (BTC sf≈2.2→×1.48, small alt sf≈0.05→×0.22)
            const double sf         = universe_.volume_scale_factor(ticker);
            const double sqrt_sf    = std::sqrt(sf);
            const double inv_sqrt   = 1.0 / sqrt_sf;

            if (strategy == "bounce") {
                auto cfg = bounce_cfg;
                cfg.stop_buffer_bps           *= inv_sqrt;
                cfg.entry_offset_bps          *= inv_sqrt;
                cfg.min_approach_speed_bps_1s *= sqrt_sf;
                cfg.no_progress_timeout_sec    = std::min(cfg.no_progress_timeout_sec * inv_sqrt, 300.0);
                auto strat = std::make_unique<BounceFromDensity>(ticker, info, cfg, system_clock_);
                strategy_engine_->add_strategy(std::move(strat));
            } else if (strategy == "breakout") {
                auto cfg = breakout_cfg;
                cfg.stop_buffer_bps          *= inv_sqrt;
                cfg.aggressive_offset_bps    *= inv_sqrt;
                cfg.min_relative_volume      *= sqrt_sf;
                cfg.no_progress_timeout_sec   = std::min(cfg.no_progress_timeout_sec * inv_sqrt, 120.0);
                cfg.post_entry_grace_sec      *= inv_sqrt;
                auto strat = std::make_unique<BreakoutEatThrough>(ticker, info, cfg, system_clock_);
                strategy_engine_->add_strategy(std::move(strat));
            } else if (strategy == "leaderlag") {
                auto cfg = leaderlag_cfg;
                cfg.stop_distance_bps        *= inv_sqrt;
                cfg.no_progress_timeout_sec   = std::min(cfg.no_progress_timeout_sec * inv_sqrt, 60.0);
                auto strat = std::make_unique<LeaderLag>(ticker, info, cfg, system_clock_);
                strategy_engine_->add_strategy(std::move(strat));
            } else if (strategy == "flushreversal") {
                auto cfg = flushreversal_cfg;
                cfg.stop_buffer_bps          *= inv_sqrt;
                cfg.entry_offset_bps         *= inv_sqrt;
                cfg.no_progress_timeout_sec   = std::min(cfg.no_progress_timeout_sec * inv_sqrt, 60.0);
                auto strat = std::make_unique<FlushReversal>(ticker, info, cfg, system_clock_);
                strategy_engine_->add_strategy(std::move(strat));
            }
        } else {
            std::string cls;
            if      (strategy == "bounce")        cls = "BounceFromDensity";
            else if (strategy == "breakout")      cls = "BreakoutEatThrough";
            else if (strategy == "leaderlag")     cls = "LeaderLag";
            else if (strategy == "flushreversal") cls = "FlushReversal";
            if (!cls.empty()) strategy_engine_->remove_strategy(ticker, cls);
            if (universe_.enabled_strategies(ticker).empty()) {
                bool has_position = false;
                for (const auto& t : executor_->get_active_trades()) {
                    if (t.plan.ticker == ticker && t.state != TradeState::Closed) {
                        has_position = true; break;
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
}

std::vector<Ticker> BotApp::init_universe_pool_(
        std::shared_ptr<OrderGateway> gateway,
        int connection_id, double exchange_volume_mult,
        const std::vector<std::string>& allow_pats,
        const std::vector<std::string>& deny_pats) {
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
                info.price_increment, info.size_increment, info.min_size, info.max_size});
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
    u_cfg.dynamic_thresholds_enabled =
        Config::get_or<bool>("universe.dynamic_thresholds.enabled", true);
    u_cfg.dynamic_thresholds_reference_volume =
        Config::get_or<double>("universe.dynamic_thresholds.reference_volume_24h_usd", 500'000'000.0);
    u_cfg.dynamic_thresholds_min_scale =
        Config::get_or<double>("universe.dynamic_thresholds.min_scale_factor", 0.15);
    u_cfg.dynamic_thresholds_max_scale =
        Config::get_or<double>("universe.dynamic_thresholds.max_scale_factor", 5.0);
    universe_.update_config(u_cfg);
    universe_.seed_candidates(all_candidates);
    return all_candidates;
}

void BotApp::start_notification_feed_(int port) {
    auto notif_ws = std::make_shared<BeastWsClient>(ioc_);
    
    // Callback fires on first ScreenerNewCoin to start processor after initial pool populated
    auto on_first_coin = [this]() {
        LOG_INFO("[Universe] First coin received from screener — starting processor");
        if (cluster_mgr_) {
            cluster_mgr_->refresh(universe_.active());
        }
        start_processor();
    };

    NotificationFeed::Config notif_cfg;
    notif_cfg.subscribe = Config::get_or<bool>("notifications.subscribe", true);
    notif_cfg.signal_levels_subscribe =
        Config::get_or<bool>("signals.level_bridge.signal_level_subscribe", true);
    notif_cfg.exchange_id = static_cast<int>(
        Config::get_or<int64_t>("notifications.exchange_id", 0));
    notif_cfg.market_type = static_cast<int>(
        Config::get_or<int64_t>("notifications.market_type", 0));
    
    notif_feed_ = std::make_unique<NotificationFeed>(
        notif_ws, universe_, notif_cfg, std::move(on_first_coin),
        signal_level_bridge_.get()); // AD3: wire bridge to route SignalLevel notifications
    notif_ws->connect("ws://127.0.0.1:" + std::to_string(port) + "/");
    notif_feed_->start();
    LOG_INFO("[Universe] NotificationFeed started (notifications={}, signal_levels={})",
             notif_cfg.subscribe, notif_cfg.signal_levels_subscribe && signal_level_bridge_ != nullptr);
}

void BotApp::schedule_signal_level_cleanup_() {
    if (!signal_level_gateway_) return;

    const auto interval_min = Config::get_or<int64_t>(
        "signals.level_bridge.cleanup_interval_min", 60);
    if (interval_min <= 0) return;

    signal_level_cleanup_timer_.expires_after(std::chrono::minutes(interval_min));
    signal_level_cleanup_timer_.async_wait([this](const boost::system::error_code& ec) {
        if (ec || shutdown_initiated_.load(std::memory_order_relaxed)) return;
        if (signal_level_gateway_) {
            try {
                signal_level_gateway_->cleanup_triggered();
                LOG_DEBUG("SignalLevelBridge: triggered server levels cleanup completed");
            } catch (const std::exception& e) {
                LOG_WARN("SignalLevelBridge: triggered server levels cleanup failed: {}", e.what());
            }
        }
        schedule_signal_level_cleanup_();
    });
}

// ═��═════════════════════════════════════════════════════════════
// init_components() — the master orchestrator
// ═══════════════════════════════════════════════════════════════

void BotApp::init_components() {
    init_config_and_logger_();

    // Strategy affinity lambdas capture exchange_mult by shared_ptr so the
    // actual multiplier (determined during connection discovery below) is
    // visible to them without a circular dependency at registration time.
    auto exchange_mult = std::make_shared<double>(1.0);

    register_strategy_affinities_(exchange_mult);

    auto ntp = std::make_shared<SntpClient>();
    clock_monitor_ = std::make_unique<ClockDriftMonitor>(ntp, ClockDriftMonitor::Config{
        .sources            = Config::get_or<std::vector<std::string>>("clock.sources", {"pool.ntp.org", "time.google.com"}),
        .check_interval     = std::chrono::seconds(Config::get_or<int64_t>("clock.check_interval_sec", 30)),
        .query_timeout      = std::chrono::milliseconds(Config::get_or<int64_t>("clock.query_timeout_ms", 1500)),
        .warn_drift_ms      = Config::get_or<int64_t>("clock.warn_drift_ms",      200),
        .max_clock_drift_ms = Config::get_or<int64_t>("clock.max_clock_drift_ms", 500),
    });
    clock_monitor_->start();

    ExternalIoContext::instance().start();
    start_dashboard_thread_();
    subscribe_signal_bus_();
    strategy_engine_ = std::make_unique<StrategyEngine>(*signal_bus_, system_clock_);
    risk_manager_ = std::make_unique<RiskManager>(universe_, news_, load_risk_config_(), system_clock_);
    load_account_state_();
    maybe_load_news_();

    http_client_ = std::make_shared<CurlHttpClient>();
    http_client_->set_timeout_ms(15000);
    auto ws = std::make_shared<BeastWsClient>(ioc_);

    MetaScalpDiscovery discovery(http_client_);
    auto port = discovery.discover();
    if (!port) throw std::runtime_error("MetaScalp not found");
    http_client_->set_timeout_ms(15000);

    auto gateway = std::make_shared<OrderGateway>(http_client_);
    gateway->set_port(*port);

    std::optional<double> cfg_exchange_mult;
    if (Config::has("universe.pool.exchange_volume_multiplier")) {
        try {
            cfg_exchange_mult = Config::get<double>("universe.pool.exchange_volume_multiplier");
        } catch (const ConfigError&) {
            LOG_WARN("[Universe] exchange_volume_multiplier has wrong type, using auto-detect");
        }
    }

    auto conn_info = discover_connection_(gateway, cfg_exchange_mult);
    *exchange_mult = conn_info.exchange_volume_mult;

    init_feed_and_executor_(ws, *port, conn_info.id, gateway);

    // SignalLevelBridge — синхронизация уровней с MetaScalp API
    if (Config::get_or<bool>("signals.level_bridge.enabled", true)) {
        signal_level_gateway_ = std::make_unique<SignalLevelGateway>(
            *http_client_, "http://localhost:" + std::to_string(*port), conn_info.id);
        SignalLevelBridge::Config bridge_cfg;
        bridge_cfg.enabled = true;
        bridge_cfg.max_server_levels = static_cast<int>(
            Config::get_or<int64_t>("signals.level_bridge.max_server_levels", 50));
        signal_level_bridge_ = std::make_unique<SignalLevelBridge>(
            *signal_level_gateway_, *signal_bus_, bridge_cfg);
        schedule_signal_level_cleanup_();
        LOG_INFO("SignalLevelBridge started — syncing levels to MetaScalp at :{}", *port);
    } else {
        LOG_INFO("SignalLevelBridge disabled by config");
    }

    setup_strategy_engine_callbacks_();

    auto strat_cfg = load_strategy_configs_();
    setup_affinity_handler_(strat_cfg.bounce, strat_cfg.breakout, strat_cfg.leaderlag, strat_cfg.flushreversal);

    ws->connect("ws://127.0.0.1:" + std::to_string(*port) + "/");
    feed_->start();

    auto allow_pats = Config::get_or<std::vector<std::string>>(
        "universe.pool.allow_patterns", std::vector<std::string>{"*USDT"});
    auto deny_pats  = Config::get_or<std::vector<std::string>>(
        "universe.pool.deny_patterns",
        std::vector<std::string>{"*UP*", "*DOWN*", "*BULL*", "*BEAR*", "*1000*"});

    pending_candidates_ = init_universe_pool_(
        gateway, conn_info.id, conn_info.exchange_volume_mult, allow_pats, deny_pats);

    // GAP-4 FIX: Wire stats lookup so affinity lambdas see live funding/spread/volatility.
    // ticker_stats_cache_ is populated every tick in tick_controllers_() from order books
    // and feed_->get_funding(). Returning nullopt for unknown tickers preserves the
    // "no stats → allow" fallback in all affinity lambdas.
    universe_.set_stats_lookup([this](const Ticker& t) -> std::optional<TickerStats> {
        auto it = ticker_stats_cache_.find(t);
        if (it == ticker_stats_cache_.end()) return std::nullopt;
        return it->second;
    });

    LOG_INFO("[Universe] Waiting for screener to populate pool ({} static-filtered candidates)",
             pending_candidates_.size());

    start_notification_feed_(*port);

    // Fallback: if screener doesn't fire within 10s, activate the full static candidate list.
    // Prevents empty dashboard when MetaScalp screener is not emitting ScreenerNewCoin.
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
        universe_.refresh_pool(pending_candidates_, system_clock_->now());
        universe_.refresh_affinity();
        if (cluster_mgr_) {
            cluster_mgr_->refresh(universe_.active());
        }
        
        // Start processor after initial pool is populated
        start_processor();
    });

    // ═══════════════════════════════════════════════════════════════
    // PipelineProcessor: Event-driven hot path (OrderBook → Submit)
    // ═══════════════════════════════════════════════════════════════
    PipelineProcessor::Config proc_cfg;
    proc_cfg.hot_path_cpu = static_cast<int>(
        Config::get_or<int64_t>("perf.hot_path_cpu", 2));
    proc_cfg.spsc_queue_capacity = static_cast<size_t>(
        Config::get_or<int64_t>("perf.spsc_queue_capacity", 65536));
    proc_cfg.feature_rate_hz = Config::get_or<double>("perf.feature_rate_hz", 20.0);
    
    processor_ = std::make_unique<PipelineProcessor>(proc_cfg);
    
    // Register pipeline components (shared across all tickers)
    processor_->set_pipeline_components(
        signal_bus_.get(),
        strategy_engine_.get(),
        risk_manager_.get(),
        executor_.get()
    );
    
    // Wire BeastWsClient raw message callback to processor queue
    ws->set_on_raw_message([this](const char* data, size_t size, uint64_t recv_ns, TraceId trace_id) {
        RawWsEvent event;
        // Skip messages that don't fit — they are snapshots/large payloads already
        // handled by the MarketDataFeed JSON path.
        if (size > RawWsEvent::kMaxPayload) return;
        event.trace_id = trace_id;
        event.recv_ns = recv_ns;
        event.payload_size = static_cast<uint32_t>(size);
        std::memcpy(event.payload.data(), data, size);

        if (!processor_->enqueue(std::move(event))) {
            LOG_WARN("[PipelineProcessor] Queue overflow — event dropped");
        }
    });
    
    // Note: Per-ticker components (OrderBook, TradeStream, LeaderTracker, FeatureExtractor)
    // will be registered dynamically in setup_affinity_handler_() when tickers are activated.
    // This happens after universe pool is populated by screener or fallback timer.
    
    LOG_INFO("[PipelineProcessor] Initialized (CPU={}, queue={})",
             proc_cfg.hot_path_cpu, proc_cfg.spsc_queue_capacity);
}

void BotApp::start_processor() {
    if (!processor_) {
        LOG_WARN("[PipelineProcessor] Cannot start — processor not initialized");
        return;
    }

    // Start processor thread only after all initial tickers are registered
    // (happens in setup_affinity_handler_ when universe pool is populated)
    feed_->set_skip_hot_path(true);
    processor_->start();
    LOG_INFO("[PipelineProcessor] Started event-driven processing thread");
}

} // namespace trade_bot
