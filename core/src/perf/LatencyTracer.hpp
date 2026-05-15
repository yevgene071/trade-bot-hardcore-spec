#pragma once

#include "numeric/HdrHistogramWrapper.hpp"
#include <chrono>
#include <string>
#include <string_view>

namespace trade_bot {

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
