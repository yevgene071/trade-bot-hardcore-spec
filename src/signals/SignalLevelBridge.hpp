#pragma once

#include "transport/SignalLevelGateway.hpp"
#include "signals/SignalBus.hpp"
#include "signals/LevelDetector.hpp"

#include <map>
#include <mutex>
#include <vector>

namespace trade_bot {

/**
 * T3-SIGLEVEL: Bridge between LevelDetector and MetaScalp server-side signal levels.
 */
class SignalLevelBridge {
public:
    struct Config {
        bool enabled{true};
        int  max_server_levels{50};
    };

    SignalLevelBridge(SignalLevelGateway& gateway, 
                     SignalBus& bus,
                     Config cfg);

    SignalLevelBridge(SignalLevelGateway& gateway, 
                     SignalBus& bus);

    /// Called by LevelDetector when a new level is formed.
    void on_level_formed(const Ticker& ticker, double price);
    
    /// Called when WS notification signal_level_triggered is received.
    void on_server_trigger(int level_id, const Ticker& ticker, double price);

private:
    SignalLevelGateway& gateway_;
    SignalBus&          bus_;
    Config              cfg_;

    struct LevelInfo {
        int id;
        Ticker ticker;
        double price;
        std::chrono::system_clock::time_point created_at;
    };

    std::map<int, LevelInfo> active_levels_;
    std::mutex               mtx_;
};

} // namespace trade_bot
