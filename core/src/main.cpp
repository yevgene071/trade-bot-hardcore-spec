#include "app/BotApp.hpp"
#include "logger/Logger.hpp"

#include <boost/asio/io_context.hpp>
#include <spdlog/spdlog.h>

#include <csignal>
#include <iostream>

using namespace trade_bot;

int main() {
    // M1: Ignore SIGPIPE so broken network connections don't kill the process.
    signal(SIGPIPE, SIG_IGN);

    try {
        boost::asio::io_context ioc;
        BotApp app(ioc);
        app.run();
        ioc.run();
    } catch (const std::exception& e) {
        if (Logger::get()) {
            LOG_CRITICAL("Startup aborted: {}", e.what());
            spdlog::shutdown();
        } else {
            std::cerr << "Fatal error: " << e.what() << std::endl;
        }
        return 1;
    }
    return 0;
}
