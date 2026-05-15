#pragma once

#include <boost/asio.hpp>
#include <thread>
#include <memory>

namespace trade_bot {

class ExternalIoContext {
public:
    static ExternalIoContext& instance() {
        static ExternalIoContext inst;
        return inst;
    }

    boost::asio::io_context& context() { return ioc_; }

    void start() {
        if (thread_.joinable()) return;
        work_guard_ = std::make_unique<boost::asio::executor_work_guard<boost::asio::io_context::executor_type>>(ioc_.get_executor());
        thread_ = std::thread([this]() { ioc_.run(); });
    }

    void stop() {
        work_guard_.reset();
        ioc_.stop();
        if (thread_.joinable()) thread_.join();
    }

private:
    ExternalIoContext() = default;
    ~ExternalIoContext() { stop(); }

    boost::asio::io_context ioc_;
    std::unique_ptr<boost::asio::executor_work_guard<boost::asio::io_context::executor_type>> work_guard_;
    std::thread thread_;
};

} // namespace trade_bot
