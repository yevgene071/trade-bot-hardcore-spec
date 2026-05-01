#include "logger/Logger.hpp"
#include "config/Config.hpp"
#include <iostream>
#include <filesystem>

int main() {
    try {
        trade_bot::Logger::init();
        
        if (!std::filesystem::exists("config.toml")) {
            LOG_CRITICAL("Config 'config.toml' not found; copy from config/config.example.toml");
            return 2;
        }
        
        trade_bot::Config::load("config.toml");

        LOG_INFO("trade_bot {} starting...", trade_bot::Config::get<std::string>("app.version"));
        
        // ... rest of the bot initialization
        
    } catch (const std::exception& e) {
        std::cerr << "Fatal error during startup: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}
