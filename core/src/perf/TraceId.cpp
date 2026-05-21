#include "perf/LatencyTracer.hpp"
#include <atomic>

namespace trade_bot {

namespace {
std::atomic<uint64_t> g_trace_id_counter{1};
}

TraceId next_trace_id() noexcept {
    return g_trace_id_counter.fetch_add(1, std::memory_order_relaxed);
}

} // namespace trade_bot
