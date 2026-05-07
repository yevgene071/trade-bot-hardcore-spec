#pragma once

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <memory>
#include <string>
#include <thread>

namespace trade_bot {

/**
 * T4-METRICS: Prometheus-compatible HTTP exporter.
 */
class MetricsExporter {
public:
    MetricsExporter(boost::asio::io_context& ioc, const std::string& address, uint16_t port, std::string auth_token = "");
    
    void start();

private:
    void do_accept();
    void on_accept(boost::beast::error_code ec, boost::asio::ip::tcp::socket socket);

    boost::asio::io_context& ioc_;
    boost::asio::ip::tcp::acceptor acceptor_;
    std::string auth_token_;
};

} // namespace trade_bot
