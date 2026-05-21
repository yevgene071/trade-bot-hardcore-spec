#include "MetricsRegistry.hpp"
#include <sstream>
#include <algorithm>
#include <cmath>
#include <mutex>

namespace trade_bot {

MetricsRegistry& MetricsRegistry::instance() {
    static MetricsRegistry inst;
    return inst;
}

void MetricsRegistry::counter_inc(const std::string& name, const std::map<std::string, std::string>& labels, double val) {
    MetricKey key{name, labels};
    // AK1: optimistic read — try shared lock first, only take unique lock on insert
    {
        std::shared_lock lock(rw_mtx_);
        auto it = counters_.find(key);
        if (it != counters_.end()) {
            double current = it->second.load();
            it->second.store(current + val);
            return;
        }
    }
    std::unique_lock lock(rw_mtx_);
    auto it = counters_.find(key);
    if (it != counters_.end()) {
        double current = it->second.load();
        it->second.store(current + val);
        return;
    }
    counters_.emplace(std::piecewise_construct, std::forward_as_tuple(key), std::forward_as_tuple(0.0));
    counters_[key].store(val);
}

void MetricsRegistry::gauge_set(const std::string& name, double val, const std::map<std::string, std::string>& labels) {
    MetricKey key{name, labels};
    {
        std::shared_lock lock(rw_mtx_);
        auto it = gauges_.find(key);
        if (it != gauges_.end()) {
            it->second.store(val);
            return;
        }
    }
    std::unique_lock lock(rw_mtx_);
    auto it = gauges_.find(key);
    if (it != gauges_.end()) {
        it->second.store(val);
        return;
    }
    gauges_.emplace(std::piecewise_construct, std::forward_as_tuple(key), std::forward_as_tuple(0.0));
    gauges_[key].store(val);
}

void MetricsRegistry::histogram_observe(const std::string& name, double val, const std::map<std::string, std::string>& labels) {
    MetricKey key{name, labels};
    {
        std::shared_lock lock(rw_mtx_);
        auto it = histograms_.find(key);
        if (it != histograms_.end()) {
            auto& h = it->second;
            h.sum += val;
            h.count++;
            std::for_each(h.buckets.begin(), h.buckets.end(), [&](double b) {
                if (val <= b) h.counts[b]++;
            });
            return;
        }
    }
    std::unique_lock lock(rw_mtx_);
    auto it = histograms_.find(key);
    if (it != histograms_.end()) {
        auto& h = it->second;
        h.sum += val;
        h.count++;
        std::for_each(h.buckets.begin(), h.buckets.end(), [&](double b) {
            if (val <= b) h.counts[b]++;
        });
        return;
    }
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
    std::shared_lock lock(rw_mtx_);
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
