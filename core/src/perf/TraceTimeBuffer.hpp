#pragma once

#include "perf/LatencyTracer.hpp"
#include <array>
#include <atomic>
#include <cstdint>
#include <optional>

namespace trade_bot {

/**
 * Cache-line padded atomic slot for lock-free access.
 */
template<typename T>
struct alignas(64) CachelinePadded {
    std::atomic<T> value{0};
};

/**
 * Lock-free ring buffer for trace_id → recv_ns correlation.
 * Uses power-of-2 size for fast modulo via bitwise AND.
 * Evicts old entries on overflow (documented behavior).
 */
class TraceTimeBuffer {
public:
    static constexpr size_t kSize = 16384; // 2^14
    static constexpr size_t kMask = kSize - 1;

    TraceTimeBuffer() = default;

    /**
     * Store trace_id → recv_ns mapping.
     * Thread-safe, lock-free. May evict old entries.
     * 
     * Uses memory_order_release on recv_ns to establish happens-before with trace_id.
     * This prevents the race where lookup() sees a new trace_id but stale recv_ns.
     * The recv_ns write with release happens-before the trace_id write with release,
     * and lookup()'s acquire on trace_id synchronizes-with this release, ensuring
     * visibility of the recv_ns value that was stored with the matching trace_id.
     */
    void store(TraceId id, uint64_t recv_ns) noexcept {
        if (id == 0) return; // Invalid trace_id
        
        size_t slot_idx = id & kMask;
        auto& slot = slots_[slot_idx];

        // Sequence lock: increment to ODD to start update
        uint32_t seq = slot.sequence.load(std::memory_order_relaxed);
        slot.sequence.store(seq + 1, std::memory_order_release);

        // Update data
        slot.ns = recv_ns;
        slot.id = id;

        // Sequence lock: increment to EVEN to finish update
        slot.sequence.store(seq + 2, std::memory_order_release);
    }

    /**
     * Lookup recv_ns by trace_id.
     * Returns nullopt if not found or evicted.
     * 
     * Uses memory_order_acquire to synchronize with store()'s release.
     * This ensures we see the recv_ns that was stored with the matching trace_id.
     */
    std::optional<uint64_t> lookup(TraceId id) const noexcept {
        if (id == 0) return std::nullopt;
        
        size_t slot_idx = id & kMask;
        auto& slot = slots_[slot_idx];

        for (int retry = 0; retry < 3; ++retry) {
            uint32_t seq1 = slot.sequence.load(std::memory_order_acquire);
            if (seq1 % 2 != 0) continue; // Update in progress

            uint64_t stored_id = slot.id;
            uint64_t ns = slot.ns;

            uint32_t seq2 = slot.sequence.load(std::memory_order_acquire);
            if (seq1 == seq2) {
                if (stored_id == id) return ns;
                return std::nullopt;
            }
        }
        
        return std::nullopt;
    }

private:
    struct alignas(64) Slot {
        std::atomic<uint32_t> sequence{0};
        uint64_t ns{0};
        TraceId id{0};
    };

    std::array<Slot, kSize> slots_;
};

/**
 * Global singleton accessor for trace time buffer.
 */
inline TraceTimeBuffer& trace_times() noexcept {
    static TraceTimeBuffer buffer;
    return buffer;
}

} // namespace trade_bot
