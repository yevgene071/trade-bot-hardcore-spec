#include "BeastWsClient.hpp"
#include "logger/Logger.hpp"
#include <boost/url.hpp>
#include <iostream>

namespace trade_bot {

BeastWsClient::BeastWsClient(net::io_context& ioc)
    : m_ioc(ioc)
    , m_resolver(net::make_strand(ioc))
    , m_reconnect_timer(ioc)
    , m_ping_timer(ioc) {}

BeastWsClient::~BeastWsClient() {
    disconnect();
}

void BeastWsClient::connect(const std::string& url) {
    m_url = url;
    m_closing = false;
    
    try {
        auto parsed_url = boost::urls::parse_uri(url);
        if (!parsed_url) {
            throw std::runtime_error("Invalid URL: " + url);
        }

        m_host = parsed_url->host_address();
        m_port = parsed_url->port();
        if (m_port.empty()) {
            m_port = (parsed_url->scheme() == "wss") ? "443" : "80";
        }
        m_target = parsed_url->path();
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
        m_ws->async_close(websocket::close_code::normal, [self = shared_from_this()](beast::error_code ec) {
            if (ec) {
                LOG_DEBUG("WS close error: {}", ec.message());
            }
        });
    }
    m_connected = false;
}

void BeastWsClient::send(std::string_view message) {
    auto msg = std::string(message);
    net::post(m_ws->get_executor(), [self = shared_from_this(), msg = std::move(msg)]() {
        bool write_in_progress = false;
        {
            std::lock_guard<std::mutex> lock(self->m_write_mutex);
            write_in_progress = !self->m_write_queue.empty();
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

    m_ws = std::make_unique<websocket::stream<beast::tcp_stream>>(net::make_strand(m_ioc));
    beast::get_lowest_layer(*m_ws).expires_after(std::chrono::seconds(30));
    beast::get_lowest_layer(*m_ws).async_connect(results, beast::bind_front_handler(&BeastWsClient::on_connect, shared_from_this()));
}

void BeastWsClient::on_connect(beast::error_code ec, tcp::resolver::results_type::endpoint_type ep) {
    boost::ignore_unused(ep);
    if (ec) {
        LOG_ERROR("WS connect error: {}", ec.message());
        schedule_reconnect();
        return;
    }

    beast::get_lowest_layer(*m_ws).expires_never();
    m_ws->set_option(websocket::stream_base::timeout::suggested(beast::role_type::client));
    
    m_ws->async_handshake(m_host, m_target, beast::bind_front_handler(&BeastWsClient::on_handshake, shared_from_this()));
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
    
    // Send any queued messages
    {
        std::lock_guard<std::mutex> lock(m_write_mutex);
        if (!m_write_queue.empty()) {
            do_write();
        }
    }
}

void BeastWsClient::do_read() {
    m_ws->async_read(m_buffer, beast::bind_front_handler(&BeastWsClient::on_read, shared_from_this()));
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

    if (m_on_message) {
        try {
            auto data = beast::buffers_to_string(m_buffer.data());
            auto j = nlohmann::json::parse(data);
            m_on_message(j);
        } catch (const std::exception& e) {
            LOG_ERROR("WS JSON parse error: {}", e.what());
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

    m_ws->async_write(net::buffer(msg), beast::bind_front_handler(&BeastWsClient::on_write, shared_from_this()));
}

void BeastWsClient::on_write(beast::error_code ec, std::size_t bytes_transferred) {
    boost::ignore_unused(bytes_transferred);

    if (ec) {
        LOG_ERROR("WS write error: {}", ec.message());
        m_connected = false;
        schedule_reconnect();
        return;
    }

    {
        std::lock_guard<std::mutex> lock(m_write_mutex);
        m_write_queue.pop();
        if (!m_write_queue.empty()) {
            do_write();
        }
    }
}

void BeastWsClient::schedule_reconnect() {
    if (m_closing) return;

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

    m_ws->async_ping({}, [self = shared_from_this()](beast::error_code ec) {
        if (ec) {
            LOG_ERROR("WS ping failed: {}", ec.message());
            self->m_connected = false;
            self->schedule_reconnect();
        } else {
            self->schedule_ping();
        }
    });
}

} // namespace trade_bot
