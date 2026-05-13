#include "DashboardServer.hpp"
#include "dashboard_html.h"
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


struct DashboardServer::Session : public std::enable_shared_from_this<DashboardServer::Session> {
    websocket::stream<beast::tcp_stream> ws;
    DashboardServer& parent;
    std::deque<std::shared_ptr<std::string>> write_queue_;
    bool writing_{false};

    Session(tcp::socket socket, DashboardServer& p) : ws(std::move(socket)), parent(p) {}

    void start(const http::request<http::string_body>& req) {
        ws.async_accept(req, [self = shared_from_this()](beast::error_code ec) {
            if (!ec) self->send_initial();
        });
    }

    void send_initial() {
        std::string payload;
        {
            std::lock_guard lock(parent.mutex_);
            payload = parent.serialize_state_locked_();
        }
        send_payload(std::move(payload));
    }

    void send_payload(std::string payload) {
        auto buf = std::make_shared<std::string>(std::move(payload));
        net::post(ws.get_executor(),
            [self = shared_from_this(), buf]() {
                self->write_queue_.push_back(buf);
                if (!self->writing_) self->do_write_();
            });
    }

    void do_write_() {
        if (write_queue_.empty()) { writing_ = false; return; }
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
    , strand_(net::make_strand(ioc_.get_executor()))
    , acceptor_(ioc, {net::ip::make_address(address), port})
    , auth_token_(std::move(auth_token)) {
    if (auth_token_.empty() && address != "127.0.0.1" && address != "::1") {
        LOG_WARN("DashboardServer bound to {} with NO auth token — anyone on "
                 "the network can read account state. Set "
                 "[dashboard].auth_token or bind to 127.0.0.1.", address);
    }
}

bool DashboardServer::authorize(std::string_view header_value) const noexcept {
    if (auth_token_.empty()) return true;
    constexpr std::string_view prefix = "Bearer ";
    if (header_value.size() <= prefix.size() ||
        header_value.substr(0, prefix.size()) != prefix) return false;
    auto presented = header_value.substr(prefix.size());
    
    bool size_match = (presented.size() == auth_token_.size());
    std::size_t compare_len = std::min(presented.size(), auth_token_.size());
    unsigned diff = 0;
    for (std::size_t i = 0; i < compare_len; ++i)
        diff |= static_cast<unsigned>(presented[i] ^ auth_token_[i]);
    
    return size_match && (diff == 0);
}

void DashboardServer::set_recorder(DumpRecorder* recorder) { recorder_ = recorder; }
bool DashboardServer::recorder_start(const std::string& path) { return recorder_ && recorder_->start(path); }
void DashboardServer::recorder_stop() { if (recorder_) recorder_->stop(); }
bool DashboardServer::recorder_active() const noexcept { return recorder_ && recorder_->is_active(); }
void DashboardServer::start() { do_accept(); }

void DashboardServer::set_selected_ticker(std::string ticker) {
    std::lock_guard lock(mutex_);
    selected_ticker_ = std::move(ticker);
    current_state_.selected_ticker = selected_ticker_;
}

std::string DashboardServer::get_selected_ticker() const noexcept {
    std::lock_guard lock(mutex_);
    return selected_ticker_;
}

void DashboardServer::update_state(const State& state) {
    std::string payload;
    std::vector<std::shared_ptr<Session>> snap;
    {
        std::lock_guard lock(mutex_);
        current_state_ = state;
        // Accumulate equity history
        if (state.server_time_unix > 0) {
            equity_history_.push_back({state.server_time_unix, state.account.equity_usd});
            while (equity_history_.size() > kMaxEquityHistory)
                equity_history_.pop_front();
        }
        if (sessions_.empty()) return;
        payload = serialize_state_locked_();
        snap.assign(sessions_.begin(), sessions_.end());
    }
    // State update: бродкаст подписчикам
    for (auto& s : snap) s->send_payload(payload);
    if (snap.size() > 0 && payload.size() > 50000) {
        LOG_TRACE("Dashboard: broadcast {} bytes to {} sessions", payload.size(), snap.size());
    }
}

void DashboardServer::update_state_async(State state) {
    // DS-10.5: Post serialization + broadcast to dashboard strand
    // so the trading loop is never blocked by JSON serialization.
    // State is taken by value and moved into the lambda to avoid
    // an extra copy of potentially large vector fields.
    net::post(strand_, [this, state = std::move(state)]() mutable {
        update_state(state);
    });
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
        {"free_balance_usd",        s.account.free_balance_usd},
        {"starting_equity_usd",     s.account.starting_equity_usd}
    };
    j["signal_counts"] = s.signal_counts;
    j["open_trades"]   = nlohmann::json::array();
    for (const auto& t : s.open_trades) {
        j["open_trades"].push_back({
            {"plan", {
                {"ticker",        t.plan.ticker},
                {"side",          static_cast<int>(t.plan.side)},
                {"strategy_name", t.plan.strategy_name},
                {"stop_price",    t.plan.stop_price},
                {"tp1_price",     t.plan.tp1_price}
            }},
            {"executed_size",   t.executed_size},
            {"avg_entry_price", t.avg_entry_price},
            {"unrealized_pnl",  t.unrealized_pnl}
        });
    }
    j["recent_journal"] = nlohmann::json::array();
    for (const auto& e : s.recent_journal) {
        j["recent_journal"].push_back({
            {"plan", {
                {"ticker",        e.plan.ticker},
                {"strategy_name", e.plan.strategy_name},
                {"size_coin",     e.plan.size_coin},
                {"side",          static_cast<int>(e.plan.side)},
                {"entry_price",   e.plan.entry_price}
            }},
            {"pnl_usd",       e.pnl_usd},
            {"exit_price",    e.exit_price},
            {"cause_of_exit", e.cause_of_exit},
            {"ts_unix_ms",    e.ts_unix_ms}
        });
    }
    j["metascalp"] = {
        {"connected",       s.metascalp.connected},
        {"latency_ms",      s.metascalp.latency_ms},
        {"connection_name", s.metascalp.connection_name}
    };
    j["universe"] = nlohmann::json::array();
    for (const auto& r : s.universe_rows) {
        j["universe"].push_back({
            {"ticker",     r.ticker},
            {"strategies", r.strategies},
            {"boosted",    r.boosted},
            {"mark_price", r.mark_price}
        });
    }
    j["recent_signals"] = nlohmann::json::array();
    for (const auto& sg : s.recent_signals) {
        j["recent_signals"].push_back({
            {"kind",       sg.kind},
            {"ticker",     sg.ticker},
            {"price",      sg.price},
            {"confidence", sg.confidence},
            {"time_str",   sg.time_str}
        });
    }
    j["strategy_stats"] = nlohmann::json::array();
    for (const auto& st : s.strategy_stats) {
        j["strategy_stats"].push_back({
            {"name",         st.name},
            {"total_trades", st.total_trades},
            {"wins",         st.wins},
            {"losses",       st.losses},
            {"total_pnl",    st.total_pnl},
            {"best_pnl",     st.best_pnl},
            {"worst_pnl",    st.worst_pnl},
            {"gross_profit", st.gross_profit},
            {"gross_loss",   st.gross_loss}
        });
    }
    j["funding_info"] = nlohmann::json::array();
    for (const auto& fi : s.funding_info) {
        j["funding_info"].push_back({
            {"ticker",             fi.ticker},
            {"rate",               fi.rate},
            {"next_funding_unix",  fi.next_funding_unix}
        });
    }
    j["risk"] = {
        {"margin_used_pct",     s.risk.margin_used_pct},
        {"exposure_pct",        s.risk.exposure_pct},
        {"total_trades_today",  s.risk.total_trades_today},
        {"consecutive_losses",  s.risk.consecutive_losses},
        {"daily_pnl_pct",       s.risk.daily_pnl_pct},
        {"current_drawdown_pct", s.risk.current_drawdown_pct}
    };
    j["recorder_active"] = s.recorder_active;
    j["recorder_path"] = s.recorder_path;
    j["server_time_unix"] = s.server_time_unix;

    // DS-11: New dashboard fields
    j["strategy_states"] = nlohmann::json::array();
    for (const auto& ss : s.strategy_states) {
        nlohmann::json sj;
        sj["ticker"]       = ss.ticker;
        sj["strategy_name"] = ss.strategy_name;
        sj["ready_state"]   = static_cast<int>(ss.ready_state);
        sj["readiness_pct"] = ss.readiness_pct;
        sj["conditions"]    = nlohmann::json::array();
        for (const auto& c : ss.conditions) {
            sj["conditions"].push_back({
                {"name",    c.name},
                {"current", c.current},
                {"target",  c.target},
                {"met",     c.met},
                {"unit",    c.unit}
            });
        }
        sj["last_reject_reason"]        = ss.last_reject_reason;
        sj["seconds_since_last_reject"] = ss.seconds_since_last_reject;
        sj["signals_last_60s"]          = ss.signals_last_60s;
        j["strategy_states"].push_back(sj);
    }

    j["chart_history"] = nlohmann::json::array();
    for (const auto& pt : s.chart_history) {
        j["chart_history"].push_back({
            {"ts",                 pt.ts_unix_ms},
            {"mid",                pt.mid},
            {"best_bid",           pt.best_bid},
            {"best_ask",           pt.best_ask},
            {"spread_bps",         pt.spread_bps},
            {"buy_vol_5s",         pt.buy_vol_5s},
            {"sell_vol_5s",        pt.sell_vol_5s},
            {"volatility_1min_bps", pt.volatility_1min_bps},
            {"tape_aggression",    pt.tape_aggression},
            {"leader_change_1s",   pt.leader_change_1s},
            {"leader_correlation", pt.leader_correlation}
        });
    }

    j["bids_top20"] = nlohmann::json::array();
    for (const auto& lv : s.bids_top20) {
        j["bids_top20"].push_back({{"price", lv.price}, {"size", lv.size}});
    }
    j["asks_top20"] = nlohmann::json::array();
    for (const auto& lv : s.asks_top20) {
        j["asks_top20"].push_back({{"price", lv.price}, {"size", lv.size}});
    }

    j["ob_mid"]        = s.ob_mid;
    j["ob_spread_bps"] = s.ob_spread_bps;
    j["ob_imbalance"]  = s.ob_imbalance;
    j["selected_ticker"] = s.selected_ticker;

    j["equity_history"] = nlohmann::json::array();
    for (const auto& pt : equity_history_) {
        j["equity_history"].push_back({{"ts", pt.first}, {"equity", pt.second}});
    }

    return j.dump();
}

void DashboardServer::do_accept() {
    acceptor_.async_accept(net::make_strand(ioc_), [this](beast::error_code ec, tcp::socket socket) {
        if (!ec) on_accept(ec, std::move(socket));
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
        LOG_TRACE("Dashboard: new session established. Total sessions: {}", sessions_.size());
    }
    ws_session->start(req);
}

void HttpSession::handle_request() {
    const auto auth_header = std::string(req->base()[http::field::authorization]);
    if (!parent.authorize(auth_header)) {
        auto res = std::make_shared<http::response<http::string_body>>(
            http::status::unauthorized, req->version());
        res->set(http::field::server,          BOOST_BEAST_VERSION_STRING);
        res->set(http::field::www_authenticate, "Bearer realm=\"trade_bot\"");
        res->set(http::field::content_type,    "text/plain");
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

    auto send_json = [&](http::status status, const std::string& body) {
        auto r = std::make_shared<http::response<http::string_body>>(status, req->version());
        r->set(http::field::server,       BOOST_BEAST_VERSION_STRING);
        r->set(http::field::content_type, "application/json");
        r->set(http::field::access_control_allow_origin, "*");
        r->keep_alive(false);
        r->body() = body;
        r->prepare_payload();
        http::async_write(stream, *r,
            [self = shared_from_this(), r](beast::error_code ec, std::size_t) {
                self->stream.socket().shutdown(tcp::socket::shutdown_send, ec);
            });
    };

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
        if (ring) for (const auto& line : ring->recent(n)) arr.push_back(line);
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

    if (req->method() == http::verb::post && req->target() == "/api/ticker/select") {
        try {
            auto body = nlohmann::json::parse(req->body());
            std::string ticker = body.value("ticker", "");
            if (!ticker.empty()) {
                parent.set_selected_ticker(ticker);
                send_json(http::status::ok, R"({"ok":true})");
                return;
            }
        } catch (...) {}
        send_json(http::status::bad_request, R"({"error":"missing ticker"})");
        return;
    }

    if (req->method() == http::verb::post && req->target() == "/api/killswitch/toggle") {
        bool active;
        {
            std::lock_guard lock(parent.mutex_);
            parent.current_state_.kill_switch_active = !parent.current_state_.kill_switch_active;
            active = parent.current_state_.kill_switch_active;
        }
        nlohmann::json j;
        j["ok"] = true;
        j["active"] = active;
        send_json(http::status::ok, j.dump());
        return;
    }

    if (req->method() == http::verb::post && req->target() == "/api/command") {
        send_json(http::status::ok, R"({"ok":true})");
        return;
    }

    auto res = std::make_shared<http::response<http::string_body>>(http::status::ok, req->version());
    res->set(http::field::server,       BOOST_BEAST_VERSION_STRING);
    res->set(http::field::content_type, "text/html; charset=utf-8");
    res->set("Content-Security-Policy",
             "default-src 'self'; script-src 'self' 'unsafe-inline'; "
             "connect-src 'self' ws: wss:; "
             "style-src 'self' 'unsafe-inline' https://fonts.googleapis.com; "
             "font-src https://fonts.gstatic.com; "
             "frame-ancestors 'none'; base-uri 'self'");
    res->set(http::field::x_frame_options, "DENY");
    res->set("X-Content-Type-Options",     "nosniff");
    res->set("Referrer-Policy",            "no-referrer");
    res->keep_alive(req->keep_alive());
    res->body().assign(kDashboardHtml, kDashboardHtmlLen);
    res->prepare_payload();

    http::async_write(stream, *res,
        [self = shared_from_this(), res](beast::error_code ec, std::size_t) {
            self->stream.socket().shutdown(tcp::socket::shutdown_send, ec);
        });
}

} // namespace trade_bot
