#pragma once

#include "numeric/HdrHistogramWrapper.hpp"
#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>

namespace trade_bot {

// Monotonic trace identifier for end-to-end latency tracking
using TraceId = uint64_t;

// Generate next trace ID (thread-safe, monotonic)
TraceId next_trace_id() noexcept;

// Helper: record delta from start_ns to now in microseconds
inline void record_delta_us(HdrHistogram& hist, uint64_t start_ns) noexcept {
    auto now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()
    ).count();
    auto delta_us = (now_ns - start_ns) / 1000;
    hist.record(delta_us);
}

/**
 * RAII tracer for measuring latency in microseconds.
 */
class LatencyTracer {
public:
    explicit LatencyTracer(HdrHistogram& hist)
        : hist_(hist)
        , start_(std::chrono::high_resolution_clock::now()) {}

    ~LatencyTracer() {
        auto end = std::chrono::high_resolution_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(end - start_).count();
        hist_.record(elapsed);
    }

private:
    HdrHistogram& hist_;
    std::chrono::high_resolution_clock::time_point start_;
};

} // namespace trade_bot
