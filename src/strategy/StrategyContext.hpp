#pragma once

#include "features/FeatureFrame.hpp"
#include "signals/Signal.hpp"

#include <unordered_map>
#include <vector>
#include <mutex>

namespace trade_bot {

/**
 * T3-PLAN: Helper that tracks current market state and recent signals.
 */
struct StrategyContext {
    FeatureFrame last_frame;

    // Recent signals: signal kind -> last signal (unordered for O(1) avg vs O(log N) map)
    std::unordered_map<SignalKind, Signal> recent_signals;

    // Full signal history for the last N seconds (vector: contiguous memory, better cache)
    std::vector<Signal> signal_history;

    void update(const FeatureFrame& frame) {
        std::lock_guard<std::mutex> lock(mtx_);
        last_frame = frame;
    }

    static constexpr std::size_t kMaxHistorySize = 1024;

    void update(const Signal& signal) {
        std::lock_guard<std::mutex> lock(mtx_);
        recent_signals[signal.kind] = signal;
        signal_history.push_back(signal);

        const auto cutoff = signal.timestamp - std::chrono::seconds(60);
        // Advance logical head without touching memory
        while (history_head_offset_ < signal_history.size() &&
               signal_history[history_head_offset_].timestamp <= cutoff) {
            ++history_head_offset_;
        }
        // Physical compaction: amortized O(1) — only when head consumes >50% and >32 slots
        if (history_head_offset_ > 32 && history_head_offset_ > signal_history.size() / 2) {
            signal_history.erase(signal_history.begin(),
                                 signal_history.begin() + static_cast<std::ptrdiff_t>(history_head_offset_));
            history_head_offset_ = 0;
        }
        // Hard cap: protect against runaway growth if compaction never triggers
        if (signal_history.size() > kMaxHistorySize) {
            const auto trim = signal_history.size() - kMaxHistorySize / 2;
            signal_history.erase(signal_history.begin(),
                                 signal_history.begin() + static_cast<std::ptrdiff_t>(trim));
            history_head_offset_ = 0;
        }
    }

    bool has_recent_signal(SignalKind kind, std::chrono::seconds max_age) const {
        std::lock_guard<std::mutex> lock(mtx_);
        auto it = recent_signals.find(kind);
        if (it == recent_signals.end()) return false;
        const auto now = std::chrono::system_clock::now();
        return (now - it->second.timestamp) <= max_age;
    }

    mutable std::mutex mtx_;

private:
    size_t history_head_offset_ = 0;
};

} // namespace trade_bot
