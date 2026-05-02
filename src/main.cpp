#include "logger/Logger.hpp"
#include "config/Config.hpp"
#include "control/KillSwitch.hpp"
#include "control/ClockDriftMonitor.hpp"
#include "control/SntpClient.hpp"

#include <chrono>
#include <filesystem>
#include <iostream>
#include <memory>
#include <thread>

int main() {
    try {
        trade_bot::Logger::init();

        // T0-KILLSWITCH: refuse to start if kill-file is present
        if (std::filesystem::exists("./killswitch")) {
            std::cerr << "Kill-switch file present, remove it to run" << std::endl;
            return 42;
        }

        if (!std::filesystem::exists("config.toml")) {
            LOG_CRITICAL("Config 'config.toml' not found; copy from config/config.example.toml");
            return 2;
        }
        trade_bot::Config::load("config.toml");

        LOG_INFO("trade_bot {} starting...",
                 trade_bot::Config::get<std::string>("app.version"));

        auto& kill_switch = trade_bot::KillSwitch::instance();
        kill_switch.start();

        // T0-CLOCK: build config from [clock] section, wire SntpClient + KillSwitch trigger.
        trade_bot::ClockDriftMonitor::Config clock_cfg;
        if (trade_bot::Config::has("clock.sources")) {
            clock_cfg.sources =
                trade_bot::Config::get<std::vector<std::string>>("clock.sources");
        }
        if (trade_bot::Config::has("clock.check_interval_sec")) {
            clock_cfg.check_interval = std::chrono::seconds{
                trade_bot::Config::get<int>("clock.check_interval_sec")};
        }
        if (trade_bot::Config::has("clock.warn_drift_ms")) {
            clock_cfg.warn_drift_ms = trade_bot::Config::get<int>("clock.warn_drift_ms");
        }
        if (trade_bot::Config::has("clock.max_clock_drift_ms")) {
            clock_cfg.max_clock_drift_ms =
                trade_bot::Config::get<int>("clock.max_clock_drift_ms");
        }

        auto ntp = std::make_shared<trade_bot::SntpClient>();
        trade_bot::ClockDriftMonitor clock_monitor(ntp, clock_cfg);
        clock_monitor.set_kill_switch([](const std::string& reason) {
            LOG_ERROR("ClockDriftMonitor escalating to kill-switch: {}", reason);
            trade_bot::KillSwitch::instance().trigger(trade_bot::KillReason::ClockDrift);
        });
        clock_monitor.start();

        // Main loop — wait until something triggers the kill-switch
        while (!kill_switch.is_triggered()) {
            std::this_thread::sleep_for(std::chrono::seconds{1});
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
