#pragma once

#include "IWsClient.hpp"
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/steady_timer.hpp>
#include <memory>
#include <thread>
#include <atomic>
#include <queue>
#include <mutex>

namespace trade_bot {

namespace beast = boost::beast;
namespace http = beast::http;
namespace websocket = beast::websocket;
namespace net = boost::asio;
using tcp = net::ip::tcp;

class BeastWsClient : public IWsClient, public std::enable_shared_from_this<BeastWsClient> {
public:
    explicit BeastWsClient(net::io_context& ioc);
    ~BeastWsClient() override;

    void connect(const std::string& url) override;
    void send(std::string_view message) override;
    void disconnect() override;

    void set_on_message(std::function<void(const nlohmann::json&)> callback) override { m_on_message = std::move(callback); }
    void set_on_close(std::function<void(int code, const std::string& reason)> callback) override { m_on_close = std::move(callback); }
    void set_on_error(std::function<void(const std::string& msg)> callback) override { m_on_error = std::move(callback); }
    void set_on_connect(std::function<void()> callback) override { m_on_connect = std::move(callback); }

    bool is_connected() const override { return m_connected; }

private:
    void do_resolve();
    void on_resolve(beast::error_code ec, tcp::resolver::results_type results);
    void on_connect(beast::error_code ec, tcp::resolver::results_type::endpoint_type ep);
    void on_handshake(beast::error_code ec);
    
    void do_read();
    void on_read(beast::error_code ec, std::size_t bytes_transferred);
    
    void do_write();
    void on_write(beast::error_code ec, std::size_t bytes_transferred);

    void schedule_reconnect();
    void schedule_ping();
    void do_ping();
    void on_ping(beast::error_code ec);

    net::io_context& m_ioc;
    tcp::resolver m_resolver;
    std::unique_ptr<websocket::stream<beast::tcp_stream>> m_ws;
    beast::flat_buffer m_buffer;
    
    std::string m_host;
    std::string m_port;
    std::string m_target;
    std::string m_url;

    std::atomic<bool> m_connected{false};
    std::atomic<bool> m_closing{false};
    int m_reconnect_delay_s = 1;

    net::steady_timer m_reconnect_timer;
    net::steady_timer m_ping_timer;

    std::queue<std::string> m_write_queue;
    std::mutex m_write_mutex;

    std::function<void(const nlohmann::json&)> m_on_message;
    std::function<void(int code, const std::string& reason)> m_on_close;
    std::function<void(const std::string& msg)> m_on_error;
    std::function<void()> m_on_connect;
};

} // namespace trade_bot
