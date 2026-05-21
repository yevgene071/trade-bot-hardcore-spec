#pragma once

#include "features/FeatureFrame.hpp"
#include "signals/Signal.hpp"

#include "absl/container/btree_map.h"
#include <vector>
#include <mutex>
#include <array>

namespace trade_bot {

/**
 * T3-PLAN: Helper that tracks current market state and recent signals.
 */
struct StrategyContext {
    FeatureFrame last_frame;

    // Recent signals: signal kind -> last signal (btree_map for deterministic iteration)
    absl::btree_map<SignalKind, Signal> recent_signals;

    // Full signal history for the last N seconds (vector: contiguous memory, better cache)
    std::vector<Signal> signal_history;

    // P0-DETERMINISM: Ring buffer of recent frames indexed by trace_id for correct
    // frame_at_entry snapshot. Strategies must capture the exact frame that triggered
    // their decision, not just last_frame (which may have advanced by the time tick() runs).
    static constexpr size_t kFrameHistorySize = 16;
    std::array<FeatureFrame, kFrameHistorySize> frame_history_;
    size_t frame_history_idx_ = 0;
    size_t frame_history_count_ = 0;

    void update(const FeatureFrame& frame) {
        std::lock_guard<std::recursive_mutex> lock(mtx_);
        last_frame = frame;
        
        // P0-DETERMINISM: Store frame in ring buffer for trace_id lookup
        frame_history_[frame_history_idx_] = frame;
        frame_history_idx_ = (frame_history_idx_ + 1) % kFrameHistorySize;
        if (frame_history_count_ < kFrameHistorySize) ++frame_history_count_;
    }

    void update(FeatureFrame&& frame) {
        std::lock_guard<std::recursive_mutex> lock(mtx_);
        last_frame = std::move(frame);
        
        // P0-DETERMINISM: Store frame in ring buffer for trace_id lookup
        frame_history_[frame_history_idx_] = last_frame;
        frame_history_idx_ = (frame_history_idx_ + 1) % kFrameHistorySize;
        if (frame_history_count_ < kFrameHistorySize) ++frame_history_count_;
    }
    
    // P0-DETERMINISM: Retrieve frame by trace_id for correct frame_at_entry snapshot
    std::optional<FeatureFrame> get_frame_by_trace_id(TraceId trace_id) const {
        std::lock_guard<std::recursive_mutex> lock(mtx_);
        for (size_t i = 0; i < frame_history_count_; ++i) {
            size_t idx = (frame_history_idx_ + kFrameHistorySize - 1 - i) % kFrameHistorySize;
            if (frame_history_[idx].derived_from == trace_id) {
                return frame_history_[idx];
            }
        }
        return std::nullopt;
    }

    static constexpr std::size_t kMaxHistorySize = 1024;

    void update(const Signal& signal) {
        std::lock_guard<std::recursive_mutex> lock(mtx_);
        recent_signals[signal.kind] = signal;
        signal_history.push_back(signal);

        const auto cutoff = signal.timestamp - std::chrono::seconds(180);
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

    // Count signals within [now - window, now], skipping logically stale entries.
    // Caller MUST hold mtx_.
    int recent_signal_count_locked(std::chrono::system_clock::time_point now,
                                    std::chrono::seconds window) const {
        const auto cutoff = now - window;
        int count = 0;
        for (size_t i = history_head_offset_; i < signal_history.size(); ++i) {
            if (signal_history[i].timestamp >= cutoff) ++count;
        }
        return count;
    }

    bool has_recent_signal(SignalKind kind, std::chrono::seconds max_age, std::chrono::system_clock::time_point now) const {
        std::lock_guard<std::recursive_mutex> lock(mtx_);
        auto it = recent_signals.find(kind);
        if (it == recent_signals.end()) return false;
        return (now - it->second.timestamp) <= max_age;
    }

    mutable std::recursive_mutex mtx_;

private:
    size_t history_head_offset_ = 0;
};

} // namespace trade_bot
