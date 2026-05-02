#pragma once

#include "risk/AccountState.hpp"
#include "trading/ActiveTrade.hpp"
#include "logger/TradeJournal.hpp"
#include "signals/Signal.hpp"

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
        AccountState account;
        std::vector<ActiveTrade> open_trades;
        std::vector<TradeJournal::Entry> recent_journal;
        std::map<std::string, int> signal_counts;
        bool kill_switch_active{false};
        std::string version;
    };

    DashboardServer(boost::asio::io_context& ioc, const std::string& address, uint16_t port);
    
    void update_state(const State& state);
    void start();

    // Internal usage
    void start_ws_session(boost::asio::ip::tcp::socket socket);

private:
    void do_accept();
    void on_accept(boost::beast::error_code ec, boost::asio::ip::tcp::socket socket);

    boost::asio::io_context& ioc_;
    boost::asio::ip::tcp::acceptor acceptor_;
    
    mutable std::mutex mutex_;
    State current_state_;
    
    // We'll store active WebSocket sessions here
    struct Session;
    std::set<std::shared_ptr<Session>> sessions_;
};

} // namespace trade_bot
