#include "KillSwitch.hpp"
#include "logger/Logger.hpp"

#include <csignal>
#include <fstream>
#include <filesystem>
#include <chrono>

namespace trade_bot {

namespace {
    std::atomic<bool> g_signal_received{false};
    void signal_handler(int signal) {
        if (signal == SIGINT || signal == SIGTERM) {
            g_signal_received = true;
        }
    }
}

std::string to_string(KillReason reason) {
    switch (reason) {
        case KillReason::None: return "None";
        case KillReason::Signal: return "Signal";
        case KillReason::File: return "File";
        case KillReason::ClockDrift: return "ClockDrift";
        case KillReason::Manual: return "Manual";
        default: return "Unknown";
    }
}

KillSwitch& KillSwitch::instance() {
    static KillSwitch inst;
    return inst;
}

KillSwitch::KillSwitch() = default;

KillSwitch::~KillSwitch() {
    stop();
}

void KillSwitch::start() {
    if (running_) return;
    
    setup_signal_handlers();
    
    // Initial check for killswitch file
    if (std::filesystem::exists(KILL_FILE)) {
        LOG_CRITICAL("Kill-switch file present at startup: {}", KILL_FILE);
        triggered_ = true;
    }

    running_ = true;
    watchdog_thread_ = std::thread(&KillSwitch::watchdog_loop, this);
}

void KillSwitch::stop() {
    running_ = false;
    if (watchdog_thread_.joinable()) {
        watchdog_thread_.join();
    }
}

bool KillSwitch::is_triggered() const {
    return triggered_.load();
}

void KillSwitch::trigger(KillReason reason) {
    if (triggered_.exchange(true)) return;

    LOG_CRITICAL("Kill-switch TRIGGERED! Reason: {}", to_string(reason));
    
    std::ofstream ofs(KILL_FILE);
    if (ofs) {
        ofs << "Reason: " << to_string(reason) << "\n";
        ofs << "Timestamp: " << std::chrono::system_clock::to_time_t(std::chrono::system_clock::now()) << "\n";
    }
}

void KillSwitch::watchdog_loop() {
    while (running_) {
        if (g_signal_received) {
            trigger(KillReason::Signal);
        }

        if (std::filesystem::exists(KILL_FILE)) {
            if (!triggered_) {
                LOG_CRITICAL("Kill-switch file detected by watchdog: {}", KILL_FILE);
                triggered_ = true;
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
}

void KillSwitch::setup_signal_handlers() {
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);
}

} // namespace trade_bot
