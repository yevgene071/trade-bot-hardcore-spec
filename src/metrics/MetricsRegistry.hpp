#pragma once

#include <string>
#include <vector>
#include <map>
#include <atomic>
#include <mutex>
#include <chrono>

namespace trade_bot {

/**
 * T4-METRICS: Thread-safe metrics registry for Prometheus export.
 */
class MetricsRegistry {
public:
    static MetricsRegistry& instance();

    // Metric types
    void counter_inc(const std::string& name, const std::map<std::string, std::string>& labels = {}, double val = 1.0);
    void gauge_set(const std::string& name, double val, const std::map<std::string, std::string>& labels = {});
    void histogram_observe(const std::string& name, double val, const std::map<std::string, std::string>& labels = {});

    // Export format
    std::string export_prometheus() const;

private:
    MetricsRegistry() = default;

    struct MetricKey {
        std::string name;
        std::map<std::string, std::string> labels;
        bool operator<(const MetricKey& other) const {
            if (name != other.name) return name < other.name;
            return labels < other.labels;
        }
    };

    struct Histogram {
        std::vector<double> buckets;
        std::map<double, uint64_t> counts;
        double sum{0.0};
        uint64_t count{0};
    };

    mutable std::mutex mtx_;
    std::map<MetricKey, std::atomic<double>> counters_;
    std::map<MetricKey, std::atomic<double>> gauges_;
    std::map<MetricKey, Histogram> histograms_;
};

} // namespace trade_bot
