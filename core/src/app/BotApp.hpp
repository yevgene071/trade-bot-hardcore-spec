#pragma once

#include "config/Config.hpp"
#include "control/KillSwitch.hpp"
#include "control/DashboardServer.hpp"
#include "features/ChartHistory.hpp"
#include "logger/Logger.hpp"
#include "logger/TradeJournal.hpp"
#include "marketdata/OrderBook.hpp"
#include "risk/AccountState.hpp"
#include "risk/NewsCalendar.hpp"
#include "risk/RiskManager.hpp"
#include "risk/TradingDay.hpp"
#include "signals/Signal.hpp"
#include "strategy/BounceFromDensity.hpp"
#include "strategy/BreakoutEatThrough.hpp"
#include "strategy/FlushReversal.hpp"
#include "strategy/LeaderLag.hpp"
#include "strategy/StrategyEngine.hpp"
#include "transport/DumpRecorder.hpp"
#include "universe/TickerUniverse.hpp"
#include "transport/IClock.hpp"
#include "transport/Clocks.hpp"
#include "numeric/Kahan.hpp"

#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>

#include "transport/IOrderGateway.hpp"

#include <atomic>
#include <chrono>
#include <deque>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace trade_bot {

// Forward declarations for unique_ptr members
class AccountStatePersister;
class ClockDriftMonitor;
class ClusterSnapshotClient;
class ClusterSnapshotManager;
class CurlHttpClient;
class FinresHandler;
class MarketDataFeed;
class MetricsExporter;
class NotificationFeed;
class OrderbookSettingsLoader;
class OrderGateway;
class SignalBus;
class TickerController;
class SignalLevelBridge;
class SignalLevelGateway;
class SntpClient;

class BotApp {
public:
    explicit BotApp(boost::asio::io_context& ioc);
    ~BotApp();

    void run();

private:
    void start_processor();

private:
    // ── Bootstrap ─────────────────────────────────────────
    void init_config_and_logger_();
    void register_strategy_affinities_(std::shared_ptr<double> exchange_mult);
    void start_dashboard_thread_();
    void subscribe_signal_bus_();
    RiskManager::Config load_risk_config_();
    void load_account_state_();
    void maybe_load_news_();

    struct ConnectionInfo {
        int    id{1};
        double exchange_volume_mult{1.0};
    };
    ConnectionInfo discover_connection_(std::shared_ptr<OrderGateway> gateway,
                                        std::optional<double> cfg_mult);
    struct StrategyConfigs {
        BounceFromDensity::Config   bounce;
        BreakoutEatThrough::Config  breakout;
        LeaderLag::Config           leaderlag;
        FlushReversal::Config       flushreversal;
    };
    StrategyConfigs load_strategy_configs_();

    // ── Wiring ────────────────────────────────────────────
    void init_components();
    void init_feed_and_executor_(std::shared_ptr<class BeastWsClient> ws, int port,
                                  int connection_id,
                                  std::shared_ptr<OrderGateway> gateway);
    void setup_strategy_engine_callbacks_();
    void dump_perf_report_();
    void setup_affinity_handler_(BounceFromDensity::Config     bounce_cfg,
                                  BreakoutEatThrough::Config   breakout_cfg,
                                  LeaderLag::Config             leaderlag_cfg,
                                  FlushReversal::Config         flushreversal_cfg);
    std::vector<Ticker> init_universe_pool_(
            std::shared_ptr<OrderGateway> gateway,
            int connection_id, double exchange_volume_mult,
            const std::vector<std::string>& allow_pats,
            const std::vector<std::string>& deny_pats);
    void start_notification_feed_(int port);

    // ── Tick / Runtime ────────────────────────────────────
    void schedule_tick();
    void tick();
    void schedule_dashboard_tick();
    void dashboard_tick();
    static std::string normalize_ticker_(const std::string& name);
    void update_account_from_exchange_();
    void check_daily_reset_();
    void tick_controllers_(std::chrono::system_clock::time_point now,
                           const std::string& leader_ticker);
    bool process_closed_trades_(std::chrono::system_clock::time_point now);
    void rebuild_slow_dash_cache_(std::chrono::system_clock::time_point now);
    void send_dashboard_state_(std::chrono::system_clock::time_point now,
                               const std::vector<struct ActiveTrade>& active,
                               double upnl_sum,
                               const std::string& leader_ticker,
                               const absl::btree_map<Ticker, double>& all_marks,
                               bool is_full_update);

    // ── Shutdown ──────────────────────────────────────────
    void handle_kill_switch_();

    // ── Members ───────────────────────────────────────────
    std::atomic<bool> shutdown_initiated_{false};
    boost::asio::io_context& ioc_;
    boost::asio::io_context dashboard_ioc_;
    std::thread dashboard_thread_;
    boost::asio::steady_timer timer_;
    boost::asio::steady_timer dashboard_timer_;
    boost::asio::steady_timer pool_fallback_timer_;

    TickerUniverse universe_;
    NewsCalendar news_;
    AccountState account_state_;
    std::string last_reset_day_;
    std::chrono::system_clock::time_point last_persist_;
    std::chrono::system_clock::time_point last_dash_update_;
    std::chrono::system_clock::time_point last_slow_dash_update_;
    std::chrono::system_clock::time_point last_ob_sanity_check_{};
    bool journal_dirty_{false};

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
    std::shared_ptr<IClock> system_clock_;  // P0 determinism: clock interface supporting replay injection
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
    std::unique_ptr<class IExecutor> executor_;
    std::shared_ptr<IOrderGateway> gateway_;
    std::unique_ptr<AccountStatePersister> persister_;
    std::unique_ptr<SignalLevelGateway> signal_level_gateway_;
    std::unique_ptr<SignalLevelBridge> signal_level_bridge_;
    std::unique_ptr<class PipelineProcessor> processor_;
    std::shared_ptr<CurlHttpClient> http_client_;
    std::unique_ptr<FinresHandler> finres_handler_;
    std::unique_ptr<OrderbookSettingsLoader> ob_settings_loader_;

    std::map<Ticker, std::unique_ptr<TickerController>> controllers_;
    std::map<Ticker, const OrderBook*> books_for_executor_;
    std::map<Ticker, std::chrono::system_clock::time_point> last_funding_times_;
    int m_connection_id_for_tick{1};
    bool live_executor_{false};
    std::vector<Ticker> pending_candidates_;
    std::map<std::string, int> signal_counts_;
    std::deque<DashboardServer::State::SignalEvent> recent_signals_;
    TradeJournal journal_{"journal"};
    int cached_consecutive_losses_{0};

    // P0 DETERMINISM: Kahan accumulators for numerically stable PnL tracking
    KahanAccumulator<double> realized_pnl_accumulator_;
    KahanAccumulator<double> equity_accumulator_;
    KahanAccumulator<double> free_balance_accumulator_;

    // WS-02: slow-tick cached fields (rebuilt every 1s, not every 100ms)
    // strategy_states, chart_history, OB snapshot are expensive to build;
    // they change slowly so we cache them and only update on slow ticks.
    std::vector<StrategyState> cached_strategy_states_;
    std::vector<ChartPoint> cached_chart_history_;
    std::vector<DensityColumn> cached_density_history_;
    std::vector<ObLevel> cached_bids_top20_;
    std::vector<ObLevel> cached_asks_top20_;
    double cached_ob_mid_{0.0};
    double cached_ob_spread_bps_{0.0};
    double cached_ob_imbalance_{0.0};

    // Per-ticker stats cache updated every tick from live data (books + funding).
    // Wired into universe_.set_stats_lookup() so affinity lambdas see live values.
    absl::btree_map<Ticker, TickerStats> ticker_stats_cache_;
    std::chrono::system_clock::time_point last_affinity_refresh_{};
};

} // namespace trade_bot
