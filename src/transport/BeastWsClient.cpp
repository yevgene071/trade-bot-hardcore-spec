#include "BeastWsClient.hpp"
#include "logger/Logger.hpp"
#include "metrics/MetricsRegistry.hpp"
#include <boost/url.hpp>
#include <boost/asio/connect.hpp>
#include <iostream>
#include <openssl/err.h>

namespace trade_bot {

BeastWsClient::BeastWsClient(net::io_context& ioc)
    : m_ioc(ioc)
    , m_resolver(net::make_strand(ioc))
    , m_reconnect_timer(ioc)
    , m_ping_timer(ioc) {
    m_ctx.set_default_verify_paths();
    // Default to verify_peer for security.
    // In local environments where MetaScalp might use self-signed certs,
    // the user should configure the context appropriately or use a config option.
    m_ctx.set_verify_mode(net::ssl::verify_peer);
}

BeastWsClient::~BeastWsClient() {
    m_closing = true;
    m_reconnect_timer.cancel();
    m_ping_timer.cancel();
    // Socket will be closed when m_ws is destroyed. 
    // We can't use shared_from_this() here.
}

void BeastWsClient::connect(const std::string& url) {
    m_url = url;
    m_closing = false;
    
    try {
        auto parsed_url = boost::urls::parse_uri(url);
        if (!parsed_url) {
            throw std::runtime_error("Invalid URL: " + url);
        }

        m_host = std::string(parsed_url->host_address());
        m_port = std::string(parsed_url->port());
        m_use_ssl = (parsed_url->scheme() == "wss");
        if (m_port.empty()) {
            m_port = m_use_ssl ? "443" : "80";
        }
        m_target = std::string(parsed_url->path());
        if (m_target.empty()) m_target = "/";
        if (parsed_url->has_query()) {
            m_target += "?" + std::string(parsed_url->query());
        }

        do_resolve();
    } catch (const std::exception& e) {
        LOG_ERROR("WS connect error: {}", e.what());
        if (m_on_error) m_on_error(e.what());
    }
}

void BeastWsClient::disconnect() {
    m_closing = true;
    m_reconnect_timer.cancel();
    m_ping_timer.cancel();

    if (m_ws && m_connected) {
        std::visit([self = shared_from_this()](auto& ws) {
            ws.async_close(websocket::close_code::normal, [self](beast::error_code ec) {
                if (ec) {
                    LOG_DEBUG("WS close error: {}", ec.message());
                }
            });
        }, *m_ws);
    }
    m_connected = false;
}

void BeastWsClient::send(std::string_view message) {
    auto msg = std::string(message);
    if (!m_ws) {
        // Not yet connected — queue the message, it will be flushed on handshake
        std::lock_guard<std::mutex> lock(m_write_mutex);
        constexpr size_t kMaxQueueSize = 1000;
        if (m_write_queue.size() >= kMaxQueueSize) m_write_queue.pop();
        m_write_queue.push(std::move(msg));
        return;
    }
    auto executor = std::visit([](auto& ws) { return ws.get_executor(); }, *m_ws);
    net::post(executor, [self = shared_from_this(), msg = std::move(msg)]() {
        bool write_in_progress = false;
        {
            std::lock_guard<std::mutex> lock(self->m_write_mutex);
            write_in_progress = !self->m_write_queue.empty();
            
            // Fix for #146: Bound the write queue to prevent OOM
            constexpr size_t kMaxQueueSize = 1000;
            if (self->m_write_queue.size() >= kMaxQueueSize) {
                self->m_write_queue.pop(); // Drop oldest
                LOG_WARN("BeastWsClient: write queue overflow, dropping oldest message");
            }
            
            self->m_write_queue.push(msg);
        }

        if (!write_in_progress && self->m_connected) {
            self->do_write();
        }
    });
}

void BeastWsClient::do_resolve() {
    m_resolver.async_resolve(m_host, m_port, beast::bind_front_handler(&BeastWsClient::on_resolve, shared_from_this()));
}

void BeastWsClient::on_resolve(beast::error_code ec, tcp::resolver::results_type results) {
    if (ec) {
        LOG_ERROR("WS resolve error: {}", ec.message());
        schedule_reconnect();
        return;
    }

    if (m_use_ssl) {
        m_ws = std::make_unique<std::variant<plain_stream, ssl_stream>>(
            std::in_place_type<ssl_stream>, net::make_strand(m_ioc), m_ctx);
    } else {
        m_ws = std::make_unique<std::variant<plain_stream, ssl_stream>>(
            std::in_place_type<plain_stream>, net::make_strand(m_ioc));
    }

    std::visit([&](auto& ws) {
        beast::get_lowest_layer(ws).expires_after(std::chrono::seconds(30));
        beast::get_lowest_layer(ws).async_connect(results, beast::bind_front_handler(&BeastWsClient::on_connect, shared_from_this()));
    }, *m_ws);
}

void BeastWsClient::on_connect(beast::error_code ec, tcp::resolver::results_type::endpoint_type ep) {
    boost::ignore_unused(ep);
    if (ec) {
        LOG_ERROR("WS connect error: {}", ec.message());
        schedule_reconnect();
        return;
    }

    std::visit([&](auto& ws) {
        beast::get_lowest_layer(ws).expires_never();
        ws.set_option(websocket::stream_base::timeout::suggested(beast::role_type::client));

        if constexpr (std::is_same_v<std::decay_t<decltype(ws)>, ssl_stream>) {
            if (!SSL_set_tlsext_host_name(ws.next_layer().native_handle(), m_host.c_str())) {
                ec = beast::error_code(static_cast<int>(::ERR_get_error()), net::error::get_ssl_category());
                LOG_ERROR("WS SSL SNI error: {}", ec.message());
                schedule_reconnect();
                return;
            }
            ws.next_layer().async_handshake(net::ssl::stream_base::client,
                beast::bind_front_handler(&BeastWsClient::on_ssl_handshake, shared_from_this()));
        } else {
            ws.async_handshake(m_host, m_target, beast::bind_front_handler(&BeastWsClient::on_handshake, shared_from_this()));
        }
    }, *m_ws);
}

void BeastWsClient::on_ssl_handshake(beast::error_code ec) {
    if (ec) {
        LOG_ERROR("WS SSL handshake error: {}", ec.message());
        schedule_reconnect();
        return;
    }

    std::get<ssl_stream>(*m_ws).async_handshake(m_host, m_target,
        beast::bind_front_handler(&BeastWsClient::on_handshake, shared_from_this()));
}

void BeastWsClient::on_handshake(beast::error_code ec) {
    if (ec) {
        LOG_ERROR("WS handshake error: {}", ec.message());
        schedule_reconnect();
        return;
    }

    m_connected = true;
    m_reconnect_delay_s = 1;
    LOG_INFO("WS connected to {}", m_url);

    if (m_on_connect) m_on_connect();

    schedule_ping();
    do_read();
    
    // Flush pre-connection queue — release lock before calling do_write()
    // to avoid deadlock (do_write also acquires m_write_mutex).
    bool has_queued = false;
    {
        std::lock_guard<std::mutex> lock(m_write_mutex);
        has_queued = !m_write_queue.empty();
    }
    if (has_queued) {
        do_write();
    }
}

void BeastWsClient::do_read() {
    std::visit([&](auto& ws) {
        ws.async_read(m_buffer, beast::bind_front_handler(&BeastWsClient::on_read, shared_from_this()));
    }, *m_ws);
}
void BeastWsClient::on_read(beast::error_code ec, std::size_t bytes_transferred) {
    boost::ignore_unused(bytes_transferred);
    if (ec) {
        if (!m_closing) {
            LOG_WARN("WS read error: {}", ec.message());
            m_connected = false;
            schedule_reconnect();
        }
        return;
    }

    LOG_TRACE("WS read {} bytes", bytes_transferred);

    if (m_on_message) {
        try {
            auto data = beast::buffers_to_string(m_buffer.data());
            LOG_TRACE("WS raw message: {}", data);

            // MetaScalp usually sends 1 JSON per WS frame. 
            // If it ever sends NDJSON, we'd need a line-based splitter here.
            auto j = nlohmann::json::parse(data);
            m_on_message(j);
        } catch (const std::exception& e) {
            LOG_ERROR("WS JSON parse error: {} | Raw data: {}", e.what(), 
                      beast::buffers_to_string(m_buffer.data()));
        }
    }

    m_buffer.consume(m_buffer.size());
    do_read();
}

void BeastWsClient::do_write() {
    std::string msg;
    {
        std::lock_guard<std::mutex> lock(m_write_mutex);
        if (m_write_queue.empty()) return;
        msg = m_write_queue.front();
    }

    std::visit([&](auto& ws) {
        ws.async_write(net::buffer(msg), beast::bind_front_handler(&BeastWsClient::on_write, shared_from_this()));
    }, *m_ws);
}

void BeastWsClient::on_write(beast::error_code ec, std::size_t bytes_transferred) {
    boost::ignore_unused(bytes_transferred);

    if (ec) {
        LOG_ERROR("WS write error: {}", ec.message());
        m_connected = false;
        schedule_reconnect();
        return;
    }

    bool more = false;
    {
        std::lock_guard<std::mutex> lock(m_write_mutex);
        m_write_queue.pop();
        more = !m_write_queue.empty();
    }
    if (more) {
        do_write();
    }
}

void BeastWsClient::schedule_reconnect() {
    if (m_closing) return;

    MetricsRegistry::instance().counter_inc("trade_bot_ws_reconnects_total");
    LOG_INFO("WS reconnecting in {}s...", m_reconnect_delay_s);
    m_reconnect_timer.expires_after(std::chrono::seconds(m_reconnect_delay_s));
    m_reconnect_timer.async_wait([self = shared_from_this()](beast::error_code ec) {
        if (!ec) {
            self->connect(self->m_url);
        }
    });

    m_reconnect_delay_s = std::min(m_reconnect_delay_s * 2, 30);
}

void BeastWsClient::schedule_ping() {
    if (m_closing || !m_connected) return;

    m_ping_timer.expires_after(std::chrono::seconds(20));
    m_ping_timer.async_wait(beast::bind_front_handler(&BeastWsClient::on_ping, shared_from_this()));
}

void BeastWsClient::on_ping(beast::error_code ec) {
    if (ec == net::error::operation_aborted) return;
    if (ec) {
        LOG_ERROR("Ping timer error: {}", ec.message());
        return;
    }
    
    do_ping();
}

void BeastWsClient::do_ping() {
    if (!m_connected) return;

    std::visit([self = shared_from_this()](auto& ws) {
        ws.async_ping({}, [self](beast::error_code ec) {
            if (ec) {
                LOG_ERROR("WS ping failed: {}", ec.message());
                self->m_connected = false;
                self->schedule_reconnect();
            } else {
                self->schedule_ping();
            }
        });
    }, *m_ws);
}

} // namespace trade_bot
