#include "logger/Logger.hpp"

int main() {
    trade_bot::Logger::init();

    LOG_INFO("trade_bot v0.0.1 starting...");
    LOG_DEBUG("Debug logging enabled");
    LOG_TRACE("Trace logging enabled (very verbose)");
    LOG_WARN("This is a warning example");
    LOG_ERROR("This is an error example with status: {}", 404);

    return 0;
}
