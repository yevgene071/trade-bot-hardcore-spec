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
    Manual,
    FeedStaleness
};

std::string to_string(KillReason reason);

class KillSwitch {
public:
    static KillSwitch& instance();

    KillSwitch(const KillSwitch&) = delete;
    KillSwitch& operator=(const KillSwitch&) = delete;

    void start();
    void stop();

    bool is_triggered() const;
    void trigger(KillReason reason);

    /// Test-only helper: clears triggered flag and removes the kill-file.
    /// Production code must NEVER untrigger a real kill-switch.
    void reset_for_test();

private:
    KillSwitch();
    ~KillSwitch();

    void watchdog_loop();
    static void setup_signal_handlers();

    std::atomic<bool> triggered_{false};
    std::atomic<bool> running_{false};
    std::thread watchdog_thread_;
    static constexpr const char* KILL_FILE = "./killswitch";
};

} // namespace trade_bot
