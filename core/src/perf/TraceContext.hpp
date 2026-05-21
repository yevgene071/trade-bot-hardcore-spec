#pragma once

#include "perf/LatencyTracer.hpp"
#include <cstdint>

namespace trade_bot {

/**
 * Thread-local trace context for propagating TraceId through the pipeline.
 */
struct TraceContext {
    TraceId trace_id{0};
    uint64_t recv_ns{0};
};

/**
 * Get current thread's trace context.
 */
inline TraceContext& current_trace_context() noexcept {
    thread_local TraceContext ctx;
    return ctx;
}

/**
 * RAII scope guard for trace context.
 * Pushes context on construction, restores previous on destruction.
 */
class TraceContextScope {
public:
    explicit TraceContextScope(TraceId trace_id, uint64_t recv_ns) noexcept
        : prev_(current_trace_context()) {
        current_trace_context() = TraceContext{trace_id, recv_ns};
    }

    explicit TraceContextScope(const TraceContext& ctx) noexcept
        : prev_(current_trace_context()) {
        current_trace_context() = ctx;
    }

    ~TraceContextScope() noexcept {
        current_trace_context() = prev_;
    }

    TraceContextScope(const TraceContextScope&) = delete;
    TraceContextScope& operator=(const TraceContextScope&) = delete;

private:
    TraceContext prev_;
};

} // namespace trade_bot
