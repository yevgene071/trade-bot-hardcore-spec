#pragma once

#include "features/FeatureFrame.hpp"
#include "signals/Signal.hpp"

#include <deque>
#include <map>
#include <vector>

namespace trade_bot {

/**
 * T3-PLAN: Helper that tracks current market state and recent signals.
 */
struct StrategyContext {
    FeatureFrame last_frame;
    
    // Recent signals: signal kind -> last signal
    std::map<SignalKind, Signal> recent_signals;
    
    // Full signal history for the last N seconds (default 60)
    std::deque<Signal> signal_history;
    
    void update(const FeatureFrame& frame) {
        last_frame = frame;
    }
    
    void update(const Signal& signal) {
        recent_signals[signal.kind] = signal;
        signal_history.push_back(signal);
        
        // Prune older signals (e.g., older than 60s)
        auto now = signal.timestamp;
        while (!signal_history.empty() && 
               (now - signal_history.front().timestamp) > std::chrono::seconds(60)) {
            signal_history.pop_front();
        }
    }
    
    bool has_recent_signal(SignalKind kind, std::chrono::seconds max_age) const {
        auto it = recent_signals.find(kind);
        if (it == recent_signals.end()) return false;
        
        auto last_ts = signal_history.empty() ? std::chrono::system_clock::time_point{} 
                                             : signal_history.back().timestamp;
        
        return (last_ts - it->second.timestamp) <= max_age;
    }
};

} // namespace trade_bot
