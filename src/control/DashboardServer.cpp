#include "DashboardServer.hpp"
#include "logger/Logger.hpp"
#include <nlohmann/json.hpp>
#include <boost/beast/version.hpp>
#include <boost/asio/strand.hpp>
#include <iostream>

namespace trade_bot {

namespace beast = boost::beast;
namespace http = beast::http;
namespace websocket = beast::websocket;
namespace net = boost::asio;
using tcp = boost::asio::ip::tcp;

static const char* kDashboardHtml = R"html(
<!DOCTYPE html>
<html>
<head>
    <title>Trade Bot Dashboard</title>
    <style>
        body { font-family: 'Segoe UI', Tahoma, Geneva, Verdana, sans-serif; background: #121212; color: #e0e0e0; margin: 20px; }
        .grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(300px, 1fr)); gap: 20px; }
        .card { background: #1e1e1e; padding: 15px; border-radius: 8px; border-left: 4px solid #3d5afe; }
        .card.risk { border-left-color: #f44336; }
        .card.signals { border-left-color: #4caf50; }
        h2 { margin-top: 0; color: #3d5afe; border-bottom: 1px solid #333; padding-bottom: 5px; }
        .stat { display: flex; justify-content: space-between; margin-bottom: 8px; }
        .val { font-weight: bold; }
        .pos-up { color: #4caf50; }
        .pos-down { color: #f44336; }
        table { width: 100%; border-collapse: collapse; margin-top: 10px; }
        th, td { text-align: left; padding: 8px; border-bottom: 1px solid #333; }
        .status-active { color: #f44336; font-weight: bold; animation: blink 1s infinite; }
        @keyframes blink { 0% { opacity: 1; } 50% { opacity: 0.3; } 100% { opacity: 1; } }
    </style>
</head>
<body>
    <h1>🚀 Trade Bot Dashboard <small id="version" style="font-size: 0.5em; color: #666;"></small></h1>
    <div id="killswitch-alert" style="display:none; background: #b71c1c; color: white; padding: 10px; margin-bottom: 20px; border-radius: 5px; font-weight: bold;">
        ⚠️ KILL-SWITCH TRIGGERED ⚠️
    </div>

    <div class="grid">
        <div class="card">
            <h2>Account & PnL</h2>
            <div class="stat"><span>Equity:</span> <span id="equity" class="val">0.00</span></div>
            <div class="stat"><span>PnL Today:</span> <span id="pnl-today" class="val">0.00</span></div>
            <div class="stat"><span>Unrealized:</span> <span id="pnl-unrealized" class="val">0.00</span></div>
            <div class="stat"><span>Free Balance:</span> <span id="free-balance" class="val">0.00</span></div>
        </div>
        
        <div class="card signals">
            <h2>Signal Counters</h2>
            <div id="signal-list"></div>
        </div>

        <div class="card">
            <h2>Open Positions</h2>
            <table id="positions-table">
                <thead><tr><th>Ticker</th><th>Side</th><th>Size</th></tr></thead>
                <tbody></tbody>
            </table>
        </div>
    </div>

    <div class="card" style="margin-top: 20px;">
        <h2>Recent Trades (Journal)</h2>
        <table id="journal-table">
            <thead><tr><th>Ticker</th><th>Strategy</th><th>Size</th><th>PnL ($)</th><th>Exit</th></tr></thead>
            <tbody></tbody>
        </table>
    </div>

    <script>
        const ws = new WebSocket('ws://' + window.location.host + '/ws');
        ws.onmessage = (event) => {
            const state = JSON.parse(event.data);
            updateUI(state);
        };
        ws.onclose = () => { console.log('WS closed, reconnecting...'); setTimeout(() => location.reload(), 5000); };

        function updateUI(s) {
            document.getElementById('version').innerText = 'v' + s.version;
            document.getElementById('equity').innerText = '$' + s.account.equity_usd.toFixed(2);
            
            const pnlToday = document.getElementById('pnl-today');
            pnlToday.innerText = (s.account.realized_pnl_today_usd >= 0 ? '+' : '') + s.account.realized_pnl_today_usd.toFixed(2);
            pnlToday.className = 'val ' + (s.account.realized_pnl_today_usd >= 0 ? 'pos-up' : 'pos-down');

            document.getElementById('pnl-unrealized').innerText = '$' + s.account.unrealized_pnl_usd.toFixed(2);
            document.getElementById('free-balance').innerText = '$' + s.account.free_balance_usd.toFixed(2);

            document.getElementById('killswitch-alert').style.display = s.kill_switch_active ? 'block' : 'none';

            // Signals
            const sigList = document.getElementById('signal-list');
            sigList.innerHTML = '';
            for (const [name, count] of Object.entries(s.signal_counts)) {
                sigList.innerHTML += `<div class="stat"><span>${name}:</span> <span class="val">${count}</span></div>`;
            }

            // Positions
            const posBody = document.querySelector('#positions-table tbody');
            posBody.innerHTML = '';
            s.open_trades.forEach(t => {
                posBody.innerHTML += `<tr>
                    <td>${t.plan.ticker}</td>
                    <td class="${t.plan.side === 1 ? 'pos-up' : 'pos-down'}">${t.plan.side === 1 ? 'BUY' : 'SELL'}</td>
                    <td>${t.executed_size.toFixed(4)}</td>
                </tr>`;
            });

            // Journal
            const jBody = document.querySelector('#journal-table tbody');
            jBody.innerHTML = '';
            s.recent_journal.forEach(e => {
                jBody.innerHTML += `<tr>
                    <td>${e.plan.ticker}</td>
                    <td>${e.plan.strategy_name}</td>
                    <td>${e.plan.size_coin}</td>
                    <td class="${e.pnl_usd >= 0 ? 'pos-up' : 'pos-down'}">${e.pnl_usd.toFixed(2)}</td>
                    <td>${e.cause_of_exit}</td>
                </tr>`;
            });
        }
    </script>
</body>
</html>
)html";

struct DashboardServer::Session : public std::enable_shared_from_this<DashboardServer::Session> {
    websocket::stream<beast::tcp_stream> ws;
    DashboardServer& parent;

    Session(tcp::socket socket, DashboardServer& p) : ws(std::move(socket)), parent(p) {}

    void start() {
        ws.async_accept([self = shared_from_this()](beast::error_code ec) {
            if (!ec) self->send_update();
        });
    }

    void send_update() {
        nlohmann::json j;
        {
            std::lock_guard lock(parent.mutex_);
            const auto& s = parent.current_state_;
            j["version"] = s.version;
            j["kill_switch_active"] = s.kill_switch_active;
            j["account"] = {
                {"equity_usd", s.account.equity_usd},
                {"realized_pnl_today_usd", s.account.realized_pnl_today_usd},
                {"unrealized_pnl_usd", s.account.unrealized_pnl_usd},
                {"free_balance_usd", s.account.free_balance_usd}
            };
            j["signal_counts"] = s.signal_counts;
            j["open_trades"] = nlohmann::json::array();
            for (const auto& t : s.open_trades) {
                j["open_trades"].push_back({
                    {"plan", {{"ticker", t.plan.ticker}, {"side", (int)t.plan.side}}},
                    {"executed_size", t.executed_size}
                });
            }
            j["recent_journal"] = nlohmann::json::array();
            for (const auto& e : s.recent_journal) {
                j["recent_journal"].push_back({
                    {"plan", {{"ticker", e.plan.ticker}, {"strategy_name", e.plan.strategy_name}, {"size_coin", e.plan.size_coin}}},
                    {"pnl_usd", e.pnl_usd},
                    {"cause_of_exit", e.cause_of_exit}
                });
            }
        }

        ws.async_write(net::buffer(j.dump()), [self = shared_from_this()](beast::error_code ec, std::size_t) {
            if (ec) {
                std::lock_guard lock(self->parent.mutex_);
                self->parent.sessions_.erase(self);
            }
        });
    }
};

struct HttpSession : public std::enable_shared_from_this<HttpSession> {
    beast::tcp_stream stream;
    beast::flat_buffer buffer;
    std::shared_ptr<http::request<http::string_body>> req;
    DashboardServer& parent;

    HttpSession(tcp::socket socket, DashboardServer& p) : stream(std::move(socket)), parent(p) {}

    void run() {
        req = std::make_shared<http::request<http::string_body>>();
        http::async_read(stream, buffer, *req, [self = shared_from_this()](beast::error_code ec, std::size_t) {
            if (ec) return;
            self->handle_request();
        });
    }

    void handle_request();
};

DashboardServer::DashboardServer(net::io_context& ioc, const std::string& address, uint16_t port)
    : ioc_(ioc), acceptor_(ioc, {net::ip::make_address(address), port}) {}

void DashboardServer::start() {
    do_accept();
}

void DashboardServer::update_state(const State& state) {
    std::lock_guard lock(mutex_);
    current_state_ = state;
    for (auto& session : sessions_) {
        session->send_update();
    }
}

void DashboardServer::do_accept() {
    acceptor_.async_accept(net::make_strand(ioc_), [this](beast::error_code ec, tcp::socket socket) {
        if (!ec) {
            on_accept(ec, std::move(socket));
        }
        do_accept();
    });
}

void DashboardServer::on_accept(beast::error_code, tcp::socket socket) {
    std::make_shared<HttpSession>(std::move(socket), *this)->run();
}

void DashboardServer::start_ws_session(tcp::socket socket) {
    auto ws_session = std::make_shared<Session>(std::move(socket), *this);
    {
        std::lock_guard lock(mutex_);
        sessions_.insert(ws_session);
    }
    ws_session->start();
}

void HttpSession::handle_request() {
    if (websocket::is_upgrade(*req)) {
        // Need to bypass private protection or move logic into DashboardServer
        // For simplicity, I'll use a hack or friend class if I had control over header.
        // Actually, I can just implement it in DashboardServer.
        parent.start_ws_session(stream.release_socket());
    } else {
        auto res = std::make_shared<http::response<http::string_body>>(http::status::ok, req->version());
        res->set(http::field::server, BOOST_BEAST_VERSION_STRING);
        res->set(http::field::content_type, "text/html");
        res->keep_alive(req->keep_alive());
        res->body() = kDashboardHtml;
        res->prepare_payload();
        
        http::async_write(stream, *res, [self = shared_from_this(), res](beast::error_code ec, std::size_t) {
            self->stream.socket().shutdown(tcp::socket::shutdown_send, ec);
        });
    }
}

} // namespace trade_bot
