#include "MetricsRegistry.hpp"
#include <sstream>
#include <algorithm>
#include <cmath>

namespace trade_bot {

MetricsRegistry& MetricsRegistry::instance() {
    static MetricsRegistry inst;
    return inst;
}

void MetricsRegistry::counter_inc(const std::string& name, const std::map<std::string, std::string>& labels, double val) {
    std::lock_guard lock(mtx_);
    MetricKey key{name, labels};
    if (!counters_.contains(key)) {
        counters_.emplace(std::piecewise_construct, std::forward_as_tuple(key), std::forward_as_tuple(0.0));
    }
    double current = counters_[key].load();
    counters_[key].store(current + val);
}

void MetricsRegistry::gauge_set(const std::string& name, double val, const std::map<std::string, std::string>& labels) {
    std::lock_guard lock(mtx_);
    MetricKey key{name, labels};
    if (!gauges_.contains(key)) {
        gauges_.emplace(std::piecewise_construct, std::forward_as_tuple(key), std::forward_as_tuple(0.0));
    }
    gauges_[key].store(val);
}

void MetricsRegistry::histogram_observe(const std::string& name, double val, const std::map<std::string, std::string>& labels) {
    std::lock_guard lock(mtx_);
    MetricKey key{name, labels};
    auto& h = histograms_[key];
    if (h.buckets.empty()) {
        h.buckets = {0.1, 1.0, 5.0, 10.0, 50.0, 100.0, 500.0, 1000.0}; // Default latency buckets in ms
        std::for_each(h.buckets.begin(), h.buckets.end(), [&](double b) { h.counts[b] = 0; });
    }
    h.sum += val;
    h.count++;
    
    std::for_each(h.buckets.begin(), h.buckets.end(), [&](double b) {
        if (val <= b) {
            h.counts[b]++;
        }
    });
}

static std::string format_labels(const std::map<std::string, std::string>& labels) {
    if (labels.empty()) return "";
    std::stringstream ss;
    ss << "{";
    for (auto it = labels.begin(); it != labels.end(); ++it) {
        if (it != labels.begin()) ss << ",";
        ss << it->first << "=\"" << it->second << "\"";
    }
    ss << "}";
    return ss.str();
}

std::string MetricsRegistry::export_prometheus() const {
    std::lock_guard lock(mtx_);
    std::stringstream ss;

    for (const auto& [key, val] : counters_) {
        ss << key.name << format_labels(key.labels) << " " << val.load() << "\n";
    }

    for (const auto& [key, val] : gauges_) {
        ss << key.name << format_labels(key.labels) << " " << val.load() << "\n";
    }

    for (const auto& [key, h] : histograms_) {
        for (double b : h.buckets) {
            std::map<std::string, std::string> l = key.labels;
            l["le"] = std::to_string(b);
            ss << key.name << "_bucket" << format_labels(l) << " " << h.counts.at(b) << "\n";
        }
        std::map<std::string, std::string> l_inf = key.labels;
        l_inf["le"] = "+Inf";
        ss << key.name << "_bucket" << format_labels(l_inf) << " " << h.count << "\n";
        ss << key.name << "_sum" << format_labels(key.labels) << " " << h.sum << "\n";
        ss << key.name << "_count" << format_labels(key.labels) << " " << h.count << "\n";
    }

    return ss.str();
}

} // namespace trade_bot
