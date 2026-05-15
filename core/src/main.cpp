#include "app/BotApp.hpp"

#include <boost/asio/io_context.hpp>

#include <iostream>

using namespace trade_bot;

int main() {
    try {
        boost::asio::io_context ioc;
        BotApp app(ioc);
        app.run();
        ioc.run();
    } catch (const std::exception& e) {
        std::cerr << "Fatal error: " << e.what() << std::endl;
        return 1;
    }
    return 0;
}
