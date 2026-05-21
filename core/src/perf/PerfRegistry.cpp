#include "perf/PerfRegistry.hpp"
#include <fmt/format.h>
#include <sstream>

namespace trade_bot {

HdrHistogram& PerfRegistry::get_or_create(std::string_view name, 
                                           int64_t max_us, 
                                           int sig_digits) {
    std::string key(name);
    
    // Thread-safe lookup and creation under mutex.
    // Hot-path callers should cache the returned reference in a static local
    // variable to avoid repeated mutex acquisition (see PerfRegistry.hpp docs).
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto it = histograms_.find(key);
    if (it != histograms_.end()) {
        return *it->second;
    }
    
    // Create and insert new histogram
    auto hist = std::make_unique<HdrHistogram>(max_us, sig_digits);
    auto* ptr = hist.get();
    histograms_[key] = std::move(hist);
    
    return *ptr;
}

void PerfRegistry::for_each(std::function<void(std::string_view, const HdrHistogram&)> fn) const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& [name, hist] : histograms_) {
        fn(name, *hist);
    }
}

std::string PerfRegistry::render_text_report() const {
    std::ostringstream oss;
    oss << "=== Performance Report ===\n";
    
    for_each([&oss](std::string_view name, const HdrHistogram& hist) {
        auto count = hist.total_count();
        if (count == 0) {
            oss << fmt::format("{}: no data\n", name);
            return;
        }
        
        auto p50 = hist.value_at_percentile(50.0);
        auto p90 = hist.value_at_percentile(90.0);
        auto p99 = hist.value_at_percentile(99.0);
        auto p999 = hist.value_at_percentile(99.9);
        auto p9999 = hist.value_at_percentile(99.99);
        auto max = hist.max();
        
        oss << fmt::format("{}: p50={} p90={} p99={} p99.9={} p99.99={} max={} count={}\n",
                          name, p50, p90, p99, p999, p9999, max, count);
    });
    
    return oss.str();
}

std::string PerfRegistry::export_prometheus(std::string_view prefix) const {
    std::ostringstream oss;
    
    for_each([&oss, prefix](std::string_view name, const HdrHistogram& hist) {
        auto count = hist.total_count();
        if (count == 0) {
            return; // Skip empty histograms
        }
        
        auto p50 = hist.value_at_percentile(50.0);
        auto p99 = hist.value_at_percentile(99.0);
        auto p999 = hist.value_at_percentile(99.9);
        auto max = hist.max();
        
        // Emit gauges for key percentiles
        oss << fmt::format("{}_{}_p50 {}\n", prefix, name, p50);
        oss << fmt::format("{}_{}_p99 {}\n", prefix, name, p99);
        oss << fmt::format("{}_{}_p999 {}\n", prefix, name, p999);
        oss << fmt::format("{}_{}_max {}\n", prefix, name, max);
        oss << fmt::format("{}_{}_count {}\n", prefix, name, count);
    });
    
    return oss.str();
}

} // namespace trade_bot
