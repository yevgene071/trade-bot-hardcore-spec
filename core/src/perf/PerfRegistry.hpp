#pragma once

#include "numeric/HdrHistogramWrapper.hpp"
#include <absl/container/flat_hash_map.h>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>

namespace trade_bot {

/**
 * Canonical stage names for performance instrumentation.
 * All instrumentation code should use these constants for consistency.
 */
inline constexpr std::string_view kStageWsToCodec = "ws_recv_to_codec_us";
inline constexpr std::string_view kStageCodecToBook = "codec_to_book_apply_us";
inline constexpr std::string_view kStageBookToFeature = "book_to_feature_us";

// Per-detector evaluation stages
inline constexpr std::string_view kStageDensityEval = "density_eval_us";
inline constexpr std::string_view kStageIcebergEval = "iceberg_eval_us";
inline constexpr std::string_view kStageLevelEval = "level_eval_us";
inline constexpr std::string_view kStageApproachEval = "approach_eval_us";
inline constexpr std::string_view kStageLeaderEval = "leader_eval_us";

// Strategy/Risk/Execution stages
inline constexpr std::string_view kStageSignalToPlan = "signal_to_plan_us";
inline constexpr std::string_view kStagePlanToRisk = "plan_to_risk_us";
inline constexpr std::string_view kStageRiskToSubmit = "risk_to_submit_us";

// End-to-end latency (book update received → order submitted)
inline constexpr std::string_view kStageEndToEnd = "end_to_end_book_to_submit_us";

// Inter-event jitter (same-stream)
inline constexpr std::string_view kStageTradeToTradeJitter = "trade_to_trade_jitter_us";
inline constexpr std::string_view kStageBookToBookJitter = "book_to_book_jitter_us";

/**
 * Global registry for performance histograms.
 * Thread-safe singleton. Histograms are registered once and stable for process lifetime.
 * 
 * Note on kStageSignalToPlan: This measures the actual strategy tick() latency
 * (signal processing → plan generation inside IStrategy::tick). Recorded once per
 * strategy invocation regardless of whether a plan is returned. Does NOT include
 * risk evaluation or order submission (those have separate histograms).
 */
class PerfRegistry {
public:
    static PerfRegistry& instance() {
        static PerfRegistry registry;
        return registry;
    }

    /**
     * Get or create a histogram by name.
     * Returns a stable reference (valid for process lifetime).
     * Thread-safe (mutex-guarded on every call).
     * 
     * PERFORMANCE: Hot-path callers should cache the returned reference in a
     * static local variable to avoid repeated mutex acquisition:
     *   static auto& hist = PerfRegistry::instance().get_or_create(kStageName);
     * C++ guarantees thread-safe initialization of function-scope statics.
     */
    HdrHistogram& get_or_create(std::string_view name, 
                                 int64_t max_us = 5'000'000, 
                                 int sig_digits = 3);

    /**
     * Iterate over all registered histograms.
     * Thread-safe (read-only access).
     */
    void for_each(std::function<void(std::string_view, const HdrHistogram&)> fn) const;

    /**
     * Render text report with p50/p90/p99/p99.9/p99.99/max/count for all histograms.
     * Format:
     *   === Performance Report ===
     *   ws_recv_to_codec_us: p50=12 p90=18 p99=25 p99.9=45 p99.99=120 max=350 count=12345
     *   ...
     */
    std::string render_text_report() const;

    /**
     * Export Prometheus metrics.
     * Emits gauges: <prefix>_<name>_p50, _p99, _p999, _max, _count
     * (Does not emit full _bucket series due to HdrHistogram's non-standard bucket layout)
     */
    std::string export_prometheus(std::string_view prefix = "trade_bot_perf") const;

    PerfRegistry(const PerfRegistry&) = delete;
    PerfRegistry& operator=(const PerfRegistry&) = delete;

private:
    PerfRegistry() = default;

    mutable std::mutex mutex_;
    absl::flat_hash_map<std::string, std::unique_ptr<HdrHistogram>> histograms_;
};

} // namespace trade_bot
