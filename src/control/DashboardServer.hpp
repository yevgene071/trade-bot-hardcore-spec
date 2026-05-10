#pragma once

#include "risk/AccountState.hpp"
#include "trading/ActiveTrade.hpp"
#include "logger/TradeJournal.hpp"
#include "signals/Signal.hpp"
#include "transport/DumpRecorder.hpp"

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/websocket.hpp>
#include <memory>
#include <string>
#include <vector>
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
        };

        AccountState account;
        std::vector<ActiveTrade> open_trades;
        std::vector<TradeJournal::Entry> recent_journal;
        std::map<std::string, int> signal_counts;
        std::vector<UniverseRow> universe_rows;
        bool kill_switch_active{false};
        std::string version;
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

private:
    void do_accept();
    void on_accept(boost::beast::error_code ec, boost::asio::ip::tcp::socket socket);

    /// Builds the JSON payload from \c current_state_. Caller MUST hold
    /// \c mutex_.
    std::string serialize_state_locked_() const;

    boost::asio::io_context& ioc_;
    boost::asio::ip::tcp::acceptor acceptor_;
    std::string auth_token_;

    mutable std::mutex mutex_;
    State current_state_;
    DumpRecorder* recorder_{nullptr};

    // We'll store active WebSocket sessions here
    struct Session;
    std::set<std::shared_ptr<Session>> sessions_;
};

} // namespace trade_bot
