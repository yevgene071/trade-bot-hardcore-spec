#pragma once

#include "risk/AccountState.hpp"
#include "trading/ActiveTrade.hpp"
#include "logger/TradeJournal.hpp"
#include "signals/Signal.hpp"
#include "strategy/IStrategy.hpp"
#include "features/ChartHistory.hpp"
#include "marketdata/OrderBook.hpp"
#include "transport/DumpRecorder.hpp"

#include <nlohmann/json.hpp>

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/websocket.hpp>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include <deque>
#include <map>
#include <mutex>
#include <set>

namespace trade_bot {

/**
 * T4-DASHBOARD: Minimal Web UI for monitoring the bot.
 * Provides a single HTML page and a WebSocket for real-time updates.
 */
class DashboardServer {
public:
    struct State {
        struct UniverseRow {
            std::string              ticker;
            std::vector<std::string> strategies;
            bool                     boosted{false};
            double                   mark_price{0.0};
        };

        struct SignalEvent {
            std::string kind;
            std::string ticker;
            double      price{0.0};
            double      confidence{0.0};
            std::string time_str; // HH:MM:SS.mmm
            std::string side;     // "Bid","Ask","Buy","Sell" from payload.side
        };

        struct StrategyStats {
            std::string name;
            int         total_trades{0};
            int         wins{0};
            int         losses{0};
            double      total_pnl{0.0};
            double      best_pnl{0.0};
            double      worst_pnl{0.0};
            double      gross_profit{0.0};
            double      gross_loss{0.0};
        };

        struct FundingInfo {
            std::string ticker;
            double      rate{0.0};
            int64_t     next_funding_unix{0};
        };

        struct RiskSnapshot {
            double margin_used_pct{0.0};
            double exposure_pct{0.0};
            int    total_trades_today{0};
            int    consecutive_losses{0}; // populated by main loop from trade tracking
            double daily_pnl_pct{0.0};
            double current_drawdown_pct{0.0};
            int    max_positions{3};      // from risk.max_concurrent_positions config
        };

        AccountState account;
        std::vector<ActiveTrade> open_trades;
        std::vector<TradeJournal::Entry> recent_journal;
        std::map<std::string, int> signal_counts;
        std::vector<UniverseRow> universe_rows;
        std::vector<SignalEvent> recent_signals; // newest first, max 60
        std::vector<StrategyStats> strategy_stats;
        std::vector<FundingInfo> funding_info;
        RiskSnapshot risk;
        bool kill_switch_active{false};
        bool recorder_active{false};
        std::string recorder_path;
        std::string version;
        int64_t server_time_unix{0};
        
        struct ConnectionStatus {
            bool connected{false};
            int64_t latency_ms{0};
            std::string connection_name;
        } metascalp;

        // DS-11: New dashboard fields
        std::vector<StrategyState>   strategy_states;
        std::vector<ChartPoint>      chart_history;
        std::vector<DensityColumn>   density_history;   // liquidity topology
        std::vector<ObLevel>         bids_top20;
        std::vector<ObLevel>         asks_top20;
        double                       ob_mid{0.0};
        double                       ob_spread_bps{0.0};
        double                       ob_imbalance{0.0};
        Ticker                       selected_ticker;    /// Monotonically increasing generation counter. Incremented on every
    /// update_state() — the frontend can skip applyServerState if the
    /// gen hasn't changed, avoiding unnecessary React re-renders.
    uint64_t                     state_gen{0};

    /// Delta-optimisation: when false, the serialiser skips heavy fields
    /// (strategy_states, chart_history, journal, funding_info, stats,
    /// equity_history) that change slowly. The frontend merges only the
    /// provided fields and preserves the rest from the last full update.
    bool                         is_full_update{true};

        struct IcebergEvent {
            int64_t     ts_ms{0};
            double      price{0.0};
            std::string side; // "BID" or "ASK"
            double      hidden_size{0.0};
        };
        std::vector<IcebergEvent> iceberg_events; // last 10
    };

    /// `auth_token` is compared against the `Authorization: Bearer <token>`
    /// header on every HTTP request and on the WS upgrade. An empty token
    /// disables auth (only safe when bound to loopback). Default bind
    /// is 127.0.0.1 in main.cpp; binding to 0.0.0.0 with empty token is
    /// caught by `Config::load()` validation.
    DashboardServer(boost::asio::io_context& ioc,
                    const std::string& address,
                    uint16_t port,
                    std::string auth_token = {});

    /// Constant-time-compare auth token against the `Authorization` header
    /// value. Public for the unit test.
    bool authorize(std::string_view authorization_header_value) const noexcept;

    /// Replace the cached state and broadcast it to every active WS session.
    /// MUST NOT be called from a Session callback — it serializes the JSON
    /// under \c mutex_ but releases the lock BEFORE invoking any
    /// async_write, so concurrent calls from the bot thread don't deadlock
    /// against a session's own serializer (#120).
    void update_state(const State& state);

    /// DS-10.5: Async version of update_state(). Posts serialization + broadcast
    /// to the dashboard io_context strand so the trading loop is never blocked
    /// by JSON serialization of large payloads (strategy_states, chart_history).
    /// Takes State by value to allow moving from the caller and avoid a
    /// second copy inside the posted lambda.
    void update_state_async(State state);

    // Wire a DumpRecorder so the dashboard can start/stop it via HTTP.
    void set_recorder(DumpRecorder* recorder);
    bool recorder_start(const std::string& path);
    void recorder_stop();
    bool recorder_active() const noexcept;

    void start();

    // Internal usage
    void start_ws_session(boost::asio::ip::tcp::socket socket,
                          const boost::beast::http::request<boost::beast::http::string_body>& req);

    /// Snapshot the current state to a JSON string. Public for the unit test
    /// that pins the deadlock-free serializer path.
    std::string serialize_state() const;

    /// Number of active WS sessions (test diagnostic).
    std::size_t session_count() const;

    /// Store ticker selected by the user via dashboard UI.
    void set_selected_ticker(std::string ticker);
    [[nodiscard]] std::string get_selected_ticker() const noexcept;

    /// Push an iceberg detection event for display on the dashboard sonar.
    /// Keeps last kMaxIcebergEvents; safe to call from any thread.
    void push_iceberg_event(int64_t ts_ms, double price, std::string side, double hidden_size);

    /// Set a handler for POST /api/command requests from the dashboard.
    /// The callback receives the command string and returns a JSON response.
    using CommandHandler = std::function<nlohmann::json(const std::string&)>;
    void set_command_handler(CommandHandler handler);

    friend struct HttpSession;

private:
    void do_accept();
    void on_accept(boost::beast::error_code ec, boost::asio::ip::tcp::socket socket);

    /// Builds the JSON payload from \c current_state_. Caller MUST hold
    /// \c mutex_.
    std::string serialize_state_locked_() const;
    
    /// Builds the binary FlatBuffers payload from \c current_state_. Caller
    /// MUST hold \c mutex_. Returns raw buffer for WebSocket binary frame.
    std::vector<uint8_t> serialize_state_binary_locked_() const;

    boost::asio::io_context& ioc_;
    boost::asio::strand<boost::asio::io_context::executor_type> strand_;
    boost::asio::ip::tcp::acceptor acceptor_;
    std::string auth_token_;

    mutable std::mutex mutex_;
    State current_state_;
    DumpRecorder* recorder_{nullptr};

    // Equity history ring buffer (1 point per update_state call, max 3600)
    static constexpr std::size_t kMaxEquityHistory = 3600;
    std::deque<std::pair<int64_t, double>> equity_history_;

    // We'll store active WebSocket sessions here
    struct Session;
    std::set<std::shared_ptr<Session>> sessions_;

    std::string selected_ticker_;
    CommandHandler command_handler_;

    /// Monotonically increasing generation counter incremented on every
    /// update_state() call. Injected into State::state_gen at serialisation
    /// time so the frontend can skip redundant applyServerState.
    uint64_t state_gen_{0};

    static constexpr std::size_t kMaxIcebergEvents = 10;

    // Phase 4: Conflation — trading loop posts state here; a 50ms timer
    // broadcasts the latest accumulated state so multiple rapid posts within
    // one 50ms window collapse into a single WebSocket write.
    bool pending_dirty_{false};
    State pending_state_;
    boost::asio::steady_timer conflation_timer_;
    void schedule_conflation_();
};

} // namespace trade_bot
