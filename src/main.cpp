#include "logger/Logger.hpp"
#include "config/Config.hpp"
#include "control/KillSwitch.hpp"
#include "control/ClockDriftMonitor.hpp"
#include <iostream>
#include <filesystem>
#include <chrono>
#include <thread>

int main() {
    try {
        trade_bot::Logger::init();
        
        // T0-KILLSWITCH: check file before start
        if (std::filesystem::exists("./killswitch")) {
            std::cerr << "Kill-switch file present, remove it to run" << std::endl;
            return 42;
        }

        if (!std::filesystem::exists("config.toml")) {
            LOG_CRITICAL("Config 'config.toml' not found; copy from config/config.example.toml");
            return 2;
        }
        
        trade_bot::Config::load("config.toml");

        LOG_INFO("trade_bot {} starting...", trade_bot::Config::get<std::string>("app.version"));
        
        auto& kill_switch = trade_bot::KillSwitch::instance();
        kill_switch.start();

        trade_bot::ClockDriftMonitor::Config clock_cfg;
        if (trade_bot::Config::has("clock.sources")) {
            clock_cfg.sources = trade_bot::Config::get<std::vector<std::string>>("clock.sources");
        }
        clock_cfg.check_interval_sec = trade_bot::Config::get<int>("clock.check_interval_sec");
        clock_cfg.warn_drift_ms = trade_bot::Config::get<int>("clock.warn_drift_ms");
        clock_cfg.max_clock_drift_ms = trade_bot::Config::get<int>("clock.max_clock_drift_ms");

        trade_bot::ClockDriftMonitor clock_monitor(clock_cfg);
        clock_monitor.start();

        // Main loop
        while (!trade_bot::KillSwitch::is_triggered()) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }

        LOG_INFO("Shutting down...");
        clock_monitor.stop();
        kill_switch.stop();
        
    } catch (const std::exception& e) {
        std::cerr << "Fatal error: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}
