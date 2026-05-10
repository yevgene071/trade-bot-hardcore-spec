#include "DashboardServer.hpp"
#include "logger/Logger.hpp"
#include <nlohmann/json.hpp>
#include <sstream>
#include <boost/beast/version.hpp>
#include <boost/asio/strand.hpp>

#include <cctype>
#include <deque>

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
    <h1>🚀 Trade Bot Dashboard <small id="version" style="font-size: 0.5em; color: #666;"></small><div id="ws-status" style="display:inline-block;margin-left:10px;padding:3px 10px;border-radius:12px;font-size:0.75em;background:#f44336;color:white;">● Disconnected</div></h1>
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

        <div class="card" style="border-left-color:#00bcd4;">
            <h2 style="color:#00bcd4;">Ticker Universe</h2>
            <div id="universe-list" style="font-size:0.9em;"></div>
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

    <div class="card" style="margin-top: 20px; border-left-color: #9c27b0;">
        <h2 style="color:#9c27b0;">Live Log <small id="log-count" style="font-size:0.7em;color:#666;"></small></h2>
        <div id="log-panel" style="font-family:monospace;font-size:0.8em;max-height:300px;overflow-y:auto;
             background:#0d0d0d;padding:8px;border-radius:4px;white-space:pre-wrap;word-break:break-all;">
            Connecting…
        </div>
    </div>

    <div class="card" style="margin-top: 20px; border-left-color: #ff9800;">
        <h2 style="color:#ff9800;">Dump Recorder</h2>
        <div class="stat">
            <span>Status:</span>
            <span id="rec-status" class="val">idle</span>
        </div>
        <div class="stat" id="rec-path-row" style="display:none;">
            <span>File:</span>
            <span id="rec-path" class="val" style="font-size:0.85em;word-break:break-all;"></span>
        </div>
        <div style="margin-top:10px; display:flex; gap:8px; align-items:center; flex-wrap:wrap;">
            <input id="rec-filename" type="text" placeholder="btcusdt_session1"
                   style="background:#2a2a2a;color:#e0e0e0;border:1px solid #555;border-radius:4px;padding:6px 10px;flex:1;min-width:160px;">
            <button id="rec-btn" onclick="toggleRecord()"
                    style="background:#ff9800;color:#000;border:none;border-radius:4px;padding:7px 18px;cursor:pointer;font-weight:bold;">
                Start Recording
            </button>
        </div>
    </div>

    <div id="last-update" style="margin-top:20px;font-size:0.8em;color:#666;">
        Waiting for first update…
    </div>

    <script>
        // DOM-API helpers — never use innerHTML for ticker/strategy fields
        // because they may carry attacker-controlled strings (#123).
        function $(id) { return document.getElementById(id); }
        function row(tbody, cells) {
            const tr = document.createElement('tr');
            for (const c of cells) {
                const td = document.createElement('td');
                if (c.cls) td.className = c.cls;
                td.textContent = c.text;          // textContent escapes everything
                tr.appendChild(td);
            }
            tbody.appendChild(tr);
        }

        function updateUI(s) {
            $('version').textContent = 'v' + s.version;
            $('equity').textContent  = '$' + Number(s.account.equity_usd || 0).toFixed(2);
            const pnl = Number(s.account.realized_pnl_today_usd || 0);
            const pnlEl = $('pnl-today');
            pnlEl.textContent = (pnl >= 0 ? '+' : '') + pnl.toFixed(2);
            pnlEl.className   = 'val ' + (pnl >= 0 ? 'pos-up' : 'pos-down');
            $('pnl-unrealized').textContent = '$' + Number(s.account.unrealized_pnl_usd || 0).toFixed(2);
            $('free-balance').textContent   = '$' + Number(s.account.free_balance_usd  || 0).toFixed(2);
            $('killswitch-alert').style.display = s.kill_switch_active ? 'block' : 'none';

            // Universe
            const uList = $('universe-list');
            uList.replaceChildren();
            const uni = s.universe || [];
            if (uni.length === 0) {
                const p = document.createElement('div');
                p.style.color = '#666';
                p.textContent = 'No active tickers';
                uList.appendChild(p);
            }
            for (const row of uni) {
                const div = document.createElement('div');
                div.className = 'stat';
                div.style.marginBottom = '6px';
                const tk = document.createElement('span');
                tk.textContent = row.ticker + (row.boosted ? ' [BOOST]' : '');
                if (row.boosted) tk.style.color = '#ffeb3b';
                const strats = document.createElement('span');
                strats.className = 'val';
                strats.style.fontSize = '0.85em';
                strats.textContent = (row.strategies && row.strategies.length)
                    ? row.strategies.join(', ') : 'none';
                div.appendChild(tk);
                div.appendChild(strats);
                uList.appendChild(div);
            }

            // Signals
            const sigList = $('signal-list');
            sigList.replaceChildren();
            for (const [name, count] of Object.entries(s.signal_counts || {})) {
                const stat = document.createElement('div'); stat.className = 'stat';
                const k = document.createElement('span'); k.textContent = name + ':';
                const v = document.createElement('span'); v.className = 'val';
                v.textContent = String(count);
                stat.appendChild(k); stat.appendChild(v);
                sigList.appendChild(stat);
            }

            // Positions
            const posBody = document.querySelector('#positions-table tbody');
            posBody.replaceChildren();
            for (const t of (s.open_trades || [])) {
                row(posBody, [
                    {text: t.plan.ticker},
                    {text: t.plan.side === 1 ? 'BUY' : 'SELL',
                     cls:  t.plan.side === 1 ? 'pos-up' : 'pos-down'},
                    {text: Number(t.executed_size || 0).toFixed(4)},
                ]);
            }

            // Journal
            const jBody = document.querySelector('#journal-table tbody');
            jBody.replaceChildren();
            for (const e of (s.recent_journal || [])) {
                const pnl = Number(e.pnl_usd || 0);
                row(jBody, [
                    {text: e.plan.ticker},
                    {text: e.plan.strategy_name},
                    {text: String(e.plan.size_coin)},
                    {text: pnl.toFixed(2), cls: pnl >= 0 ? 'pos-up' : 'pos-down'},
                    {text: e.cause_of_exit},
                ]);
            }

            $('last-update').textContent =
                'Last update: ' + new Date().toISOString();

            // Recorder status
            const rec = s.dump_recorder || {};
            _recording = !!rec.active;  // Sync with server state
            const isRec = !!rec.active;
            $('rec-status').textContent = isRec ? '● RECORDING' : 'idle';
            $('rec-status').style.color = isRec ? '#f44336' : '#aaa';
            $('rec-path-row').style.display = isRec ? '' : 'none';
            if (isRec) $('rec-path').textContent = rec.path || '';
            $('rec-btn').textContent = isRec ? 'Stop Recording' : 'Start Recording';
            $('rec-btn').style.background = isRec ? '#f44336' : '#ff9800';
        }

        let _recording = false;
        function toggleRecord() {
            if (_recording) {
                fetch('/api/dump/stop', {method:'POST'}).catch(console.error);
            } else {
                const name = $('rec-filename').value.trim() || 'dump';
                fetch('/api/dump/start', {
                    method: 'POST',
                    headers: {'Content-Type': 'application/json'},
                    body: JSON.stringify({filename: name})
                }).catch(console.error);
            }
            _recording = !_recording;
        }

        // WS reconnect with exponential backoff (#124). Replaces the previous
        // location.reload() which dropped console history & scroll position.
        let backoff = 500;
        function connect() {
            const ws = new WebSocket('ws://' + window.location.host + '/ws');
            ws.onopen    = () => { backoff = 500; $('ws-status').textContent = '● Live'; $('ws-status').style.background = '#4caf50'; };
            ws.onmessage = (ev) => { try { updateUI(JSON.parse(ev.data)); }
                                     catch (e) { console.error('bad payload', e); } };
            ws.onclose   = () => {
                $('ws-status').textContent = '● Reconnecting…'; $('ws-status').style.background = '#ff9800';
                backoff = Math.min(backoff * 2, 30000);
                setTimeout(connect, backoff);
            };
            ws.onerror   = () => ws.close();
        }
        connect();

        // Log panel polling
        const LOG_COLORS = {
            'error':    '#f44336',
            'critical': '#ff1744',
            'warning':  '#ff9800',
            'warn':     '#ff9800',
            'info':     '#80cbc4',
            'debug':    '#888',
            'trace':    '#555',
        };
        let _logScrollLock = false;
        const logPanel = $('log-panel');
        logPanel.addEventListener('scroll', () => {
            _logScrollLock = logPanel.scrollTop < logPanel.scrollHeight - logPanel.clientHeight - 20;
        });

        function fetchLogs() {
            fetch('/api/logs?n=80').then(r => r.json()).then(lines => {
                logPanel.replaceChildren();
                for (const line of lines) {
                    const span = document.createElement('span');
                    const lvlMatch = line.match(/\[(error|critical|warning|warn|info|debug|trace)\]/i);
                    span.style.color = lvlMatch ? (LOG_COLORS[lvlMatch[1].toLowerCase()] || '#e0e0e0') : '#e0e0e0';
                    span.textContent = line + '\n';
                    logPanel.appendChild(span);
                }
                $('log-count').textContent = lines.length + ' lines';
                if (!_logScrollLock)
                    logPanel.scrollTop = logPanel.scrollHeight;
            }).catch(() => {});
        }
        fetchLogs();
        setInterval(fetchLogs, 2000);
    </script>
</body>
</html>
)html";

struct DashboardServer::Session : public std::enable_shared_from_this<DashboardServer::Session> {
    websocket::stream<beast::tcp_stream> ws;
    DashboardServer& parent;
    // Per-session write pipeline. Boost.Beast requires only one async_write
    // in flight per stream, so we serialize through a queue (#122). All
    // queue manipulation runs on the WS strand.
    std::deque<std::shared_ptr<std::string>> write_queue_;
    bool writing_{false};

    Session(tcp::socket socket, DashboardServer& p) : ws(std::move(socket)), parent(p) {}

    void start(const http::request<http::string_body>& req) {
        ws.async_accept(req, [self = shared_from_this()](beast::error_code ec) {
            if (!ec) self->send_initial();
        });
    }

    /// First push after WS handshake — builds payload under the lock and
    /// releases it before async_write to avoid #120-class re-entrancy.
    void send_initial() {
        std::string payload;
        {
            std::lock_guard lock(parent.mutex_);
            payload = parent.serialize_state_locked_();
        }
        send_payload(std::move(payload));
    }

    /// Push a payload onto the write queue and start the writer if idle.
    /// Posted onto the stream's executor so external threads (the bot's
    /// update_state) cannot race with the strand-bound writer (#122).
    void send_payload(std::string payload) {
        auto buf = std::make_shared<std::string>(std::move(payload));
        net::post(ws.get_executor(),
            [self = shared_from_this(), buf]() {
                self->write_queue_.push_back(buf);
                if (!self->writing_) self->do_write_();
            });
    }

    void do_write_() {
        if (write_queue_.empty()) {
            writing_ = false;
            return;
        }
        writing_ = true;
        auto& front = write_queue_.front();
        ws.async_write(net::buffer(*front),
            [self = shared_from_this()](beast::error_code ec, std::size_t) {
                if (ec) {
                    self->write_queue_.clear();
                    self->writing_ = false;
                    std::lock_guard lock(self->parent.mutex_);
                    self->parent.sessions_.erase(self);
                    return;
                }
                self->write_queue_.pop_front();
                self->do_write_();
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

DashboardServer::DashboardServer(net::io_context& ioc,
                                 const std::string& address,
                                 uint16_t port,
                                 std::string auth_token)
    : ioc_(ioc)
    , acceptor_(ioc, {net::ip::make_address(address), port})
    , auth_token_(std::move(auth_token)) {
    if (auth_token_.empty() && address != "127.0.0.1" && address != "::1") {
        LOG_WARN("DashboardServer bound to {} with NO auth token — anyone on "
                 "the network can read account state. Set "
                 "[dashboard].auth_token or bind to 127.0.0.1.", address);
    }
}

bool DashboardServer::authorize(std::string_view header_value) const noexcept {
    if (auth_token_.empty()) {
        return true;     // operator opted in to auth-less loopback mode
    }
    constexpr std::string_view prefix = "Bearer ";
    if (header_value.size() <= prefix.size() ||
        header_value.substr(0, prefix.size()) != prefix) {
        return false;
    }
    auto presented = header_value.substr(prefix.size());
    if (presented.size() != auth_token_.size()) return false;
    // constant-time compare
    unsigned diff = 0;
    for (std::size_t i = 0; i < presented.size(); ++i) {
        diff |= static_cast<unsigned>(presented[i] ^ auth_token_[i]);
    }
    return diff == 0;
}

void DashboardServer::set_recorder(DumpRecorder* recorder) {
    recorder_ = recorder;
}

bool DashboardServer::recorder_start(const std::string& path) {
    return recorder_ && recorder_->start(path);
}

void DashboardServer::recorder_stop() {
    if (recorder_) recorder_->stop();
}

bool DashboardServer::recorder_active() const noexcept {
    return recorder_ && recorder_->is_active();
}

void DashboardServer::start() {
    do_accept();
}

void DashboardServer::update_state(const State& state) {
    // Build the payload + snapshot the live session set under the lock,
    // then drop the lock before broadcasting. Previously the loop called
    // Session::send_update which itself acquired parent.mutex_, deadlocking
    // a non-recursive std::mutex (#120). It also held the lock across
    // async_write, blocking unrelated update_state callers behind socket
    // I/O. Both go away with the snapshot pattern.
    std::string payload;
    std::vector<std::shared_ptr<Session>> sessions_snap;
    {
        std::lock_guard lock(mutex_);
        current_state_ = state;
        payload = serialize_state_locked_();
        sessions_snap.assign(sessions_.begin(), sessions_.end());
    }

    LOG_TRACE("Dashboard update: {} bytes, {} sessions", payload.size(), sessions_snap.size());

    for (auto& s : sessions_snap) {
        s->send_payload(payload);
    }
}

std::string DashboardServer::serialize_state() const {
    std::lock_guard lock(mutex_);
    return serialize_state_locked_();
}

std::size_t DashboardServer::session_count() const {
    std::lock_guard lock(mutex_);
    return sessions_.size();
}

std::string DashboardServer::serialize_state_locked_() const {
    nlohmann::json j;
    const auto& s = current_state_;
    j["version"]            = s.version;
    j["kill_switch_active"] = s.kill_switch_active;
    j["account"] = {
        {"equity_usd",              s.account.equity_usd},
        {"realized_pnl_today_usd",  s.account.realized_pnl_today_usd},
        {"unrealized_pnl_usd",      s.account.unrealized_pnl_usd},
        {"free_balance_usd",        s.account.free_balance_usd}
    };
    j["signal_counts"] = s.signal_counts;
    j["open_trades"]   = nlohmann::json::array();
    for (const auto& t : s.open_trades) {
        j["open_trades"].push_back({
            {"plan", {{"ticker", t.plan.ticker}, {"side", static_cast<int>(t.plan.side)}}},
            {"executed_size", t.executed_size}
        });
    }
    j["recent_journal"] = nlohmann::json::array();
    for (const auto& e : s.recent_journal) {
        j["recent_journal"].push_back({
            {"plan", {{"ticker",        e.plan.ticker},
                      {"strategy_name", e.plan.strategy_name},
                      {"size_coin",     e.plan.size_coin}}},
            {"pnl_usd",       e.pnl_usd},
            {"cause_of_exit", e.cause_of_exit}
        });
    }
    j["dump_recorder"] = {
        {"active", recorder_ && recorder_->is_active()},
        {"path",   recorder_ ? recorder_->path() : ""}
    };
    j["universe"] = nlohmann::json::array();
    for (const auto& r : s.universe_rows) {
        j["universe"].push_back({
            {"ticker",     r.ticker},
            {"strategies", r.strategies},
            {"boosted",    r.boosted}
        });
    }
    return j.dump();
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

void DashboardServer::start_ws_session(tcp::socket socket,
                                        const http::request<http::string_body>& req) {
    auto ws_session = std::make_shared<Session>(std::move(socket), *this);
    {
        std::lock_guard lock(mutex_);
        sessions_.insert(ws_session);
    }
    ws_session->start(req);
}

void HttpSession::handle_request() {
    const auto auth_header = std::string(req->base()[http::field::authorization]);
    if (!parent.authorize(auth_header)) {
        auto res = std::make_shared<http::response<http::string_body>>(
            http::status::unauthorized, req->version());
        res->set(http::field::server,        BOOST_BEAST_VERSION_STRING);
        res->set(http::field::www_authenticate, "Bearer realm=\"trade_bot\"");
        res->set(http::field::content_type,  "text/plain");
        res->keep_alive(false);
        res->body() = "401 Unauthorized\n";
        res->prepare_payload();
        http::async_write(stream, *res,
            [self = shared_from_this(), res](beast::error_code ec, std::size_t) {
                self->stream.socket().shutdown(tcp::socket::shutdown_send, ec);
            });
        return;
    }

    if (websocket::is_upgrade(*req)) {
        parent.start_ws_session(stream.release_socket(), *req);
        return;
    }

    // Dump recorder API
    auto send_json = [&](http::status status, const std::string& body) {
        auto r = std::make_shared<http::response<http::string_body>>(status, req->version());
        r->set(http::field::server,        BOOST_BEAST_VERSION_STRING);
        r->set(http::field::content_type,  "application/json");
        r->set(http::field::access_control_allow_origin, "*");
        r->keep_alive(false);
        r->body() = body;
        r->prepare_payload();
        http::async_write(stream, *r,
            [self = shared_from_this(), r](beast::error_code ec, std::size_t) {
                self->stream.socket().shutdown(tcp::socket::shutdown_send, ec);
            });
    };

    // /api/logs?n=<count>
    if (req->method() == http::verb::get &&
        std::string(req->target()).starts_with("/api/logs")) {
        std::size_t n = 80;
        auto target = std::string(req->target());
        auto pos = target.find("n=");
        if (pos != std::string::npos) {
            try { n = std::stoul(target.substr(pos + 2)); } catch (...) {}
        }
        n = std::min(n, std::size_t{200});
        auto ring = trade_bot::Logger::ring();
        nlohmann::json arr = nlohmann::json::array();
        if (ring) {
            for (const auto& line : ring->recent(n))
                arr.push_back(line);
        }
        send_json(http::status::ok, arr.dump());
        return;
    }

    if (req->method() == http::verb::get && req->target() == "/api/state") {
        send_json(http::status::ok, parent.serialize_state());
        return;
    }

    if (req->method() == http::verb::post && req->target() == "/api/dump/start") {
        std::string filename = "dump";
        try {
            auto body = nlohmann::json::parse(req->body());
            filename = body.value("filename", filename);
        } catch (...) {}
        // Sanitize: keep only alnum, dash, underscore, dot
        std::string safe;
        for (char c : filename) {
            if (std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' || c == '.')
                safe += c;
        }
        if (safe.empty()) safe = "dump";
        std::string path = "replay/dumps/" + safe + ".ndjson";
        bool ok = parent.recorder_start(path);
        send_json(ok ? http::status::ok : http::status::internal_server_error,
                  ok ? R"({"ok":true})" : R"({"error":"could not open file"})");
        return;
    }

    if (req->method() == http::verb::post && req->target() == "/api/dump/stop") {
        parent.recorder_stop();
        send_json(http::status::ok, R"({"ok":true})");
        return;
    }

    auto res = std::make_shared<http::response<http::string_body>>(http::status::ok, req->version());
    res->set(http::field::server,        BOOST_BEAST_VERSION_STRING);
    res->set(http::field::content_type,  "text/html; charset=utf-8");
    // Strict CSP: no inline scripts allowed beyond what the page itself
    // ships, no remote sources, no framing. Closes #123.
    res->set("Content-Security-Policy",
             "default-src 'self'; script-src 'self' 'unsafe-inline'; "
             "connect-src 'self' ws: wss:; "
             "style-src 'self' 'unsafe-inline'; "
             "frame-ancestors 'none'; base-uri 'self'");
    res->set(http::field::x_frame_options,        "DENY");
    res->set("X-Content-Type-Options",            "nosniff");
    res->set("Referrer-Policy",                   "no-referrer");
    res->keep_alive(req->keep_alive());
    res->body() = kDashboardHtml;
    res->prepare_payload();

    http::async_write(stream, *res, [self = shared_from_this(), res](beast::error_code ec, std::size_t) {
        self->stream.socket().shutdown(tcp::socket::shutdown_send, ec);
    });
}

} // namespace trade_bot
