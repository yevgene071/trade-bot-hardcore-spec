#pragma once

#include <atomic>
#include <string>
#include <thread>

namespace trade_bot {

enum class KillReason {
    None,
    Signal,
    File,
    ClockDrift,
    Manual
};

std::string to_string(KillReason reason);

class KillSwitch {
public:
    static KillSwitch& instance();

    KillSwitch(const KillSwitch&) = delete;
    KillSwitch& operator=(const KillSwitch&) = delete;

    void start();
    void stop();

    static bool is_triggered();
    static void trigger(KillReason reason);

private:
    KillSwitch();
    ~KillSwitch();

    void watchdog_loop();
    void setup_signal_handlers();

    std::atomic<bool> triggered_{false};
    std::atomic<bool> running_{false};
    std::thread watchdog_thread_;
    static constexpr const char* KILL_FILE = "./killswitch";
};

} // namespace trade_bot
