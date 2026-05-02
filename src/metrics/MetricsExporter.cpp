#include "MetricsExporter.hpp"
#include "MetricsRegistry.hpp"
#include "logger/Logger.hpp"
#include <boost/beast/version.hpp>
#include <boost/asio/strand.hpp>

namespace trade_bot {

namespace beast = boost::beast;
namespace http = beast::http;
namespace net = boost::asio;
using tcp = boost::asio::ip::tcp;

struct MetricsSession : public std::enable_shared_from_this<MetricsSession> {
    beast::tcp_stream stream;
    beast::flat_buffer buffer;
    std::shared_ptr<http::request<http::string_body>> req;

    MetricsSession(tcp::socket socket) : stream(std::move(socket)) {}

    void run() {
        req = std::make_shared<http::request<http::string_body>>();
        http::async_read(stream, buffer, *req, [self = shared_from_this()](beast::error_code ec, std::size_t) {
            if (ec) return;
            self->handle_request();
        });
    }

    void handle_request() {
        if (req->target() == "/metrics") {
            auto res = std::make_shared<http::response<http::string_body>>(http::status::ok, req->version());
            res->set(http::field::server, BOOST_BEAST_VERSION_STRING);
            res->set(http::field::content_type, "text/plain; version=0.0.4");
            res->keep_alive(req->keep_alive());
            res->body() = MetricsRegistry::instance().export_prometheus();
            res->prepare_payload();
            
            http::async_write(stream, *res, [self = shared_from_this(), res](beast::error_code ec, std::size_t) {
                self->stream.socket().shutdown(tcp::socket::shutdown_send, ec);
            });
        } else {
            auto res = std::make_shared<http::response<http::string_body>>(http::status::not_found, req->version());
            res->prepare_payload();
            http::async_write(stream, *res, [self = shared_from_this(), res](beast::error_code ec, std::size_t) {
                self->stream.socket().shutdown(tcp::socket::shutdown_send, ec);
            });
        }
    }
};

MetricsExporter::MetricsExporter(net::io_context& ioc, const std::string& address, uint16_t port)
    : ioc_(ioc), acceptor_(ioc, {net::ip::make_address(address), port}) {}

void MetricsExporter::start() {
    do_accept();
}

void MetricsExporter::do_accept() {
    acceptor_.async_accept(net::make_strand(ioc_), [this](beast::error_code ec, tcp::socket socket) {
        if (!ec) {
            std::make_shared<MetricsSession>(std::move(socket))->run();
        }
        do_accept();
    });
}

} // namespace trade_bot
