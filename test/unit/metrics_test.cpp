#include "metrics/MetricsRegistry.hpp"
#include <gtest/gtest.h>
#include <regex>

using namespace trade_bot;

TEST(MetricsTest, CounterWorks) {
    auto& r = MetricsRegistry::instance();
    r.counter_inc("test_counter", {{"label", "v1"}});
    r.counter_inc("test_counter", {{"label", "v1"}}, 2.5);
    
    std::string exported = r.export_prometheus();
    EXPECT_TRUE(exported.find("test_counter{label=\"v1\"} 3.5") != std::string::npos);
}

TEST(MetricsTest, GaugeWorks) {
    auto& r = MetricsRegistry::instance();
    r.gauge_set("test_gauge", 42.0, {{"label", "v2"}});
    
    std::string exported = r.export_prometheus();
    EXPECT_TRUE(exported.find("test_gauge{label=\"v2\"} 42") != std::string::npos);
    
    r.gauge_set("test_gauge", 100.0, {{"label", "v2"}});
    exported = r.export_prometheus();
    EXPECT_TRUE(exported.find("test_gauge{label=\"v2\"} 100") != std::string::npos);
}

TEST(MetricsTest, HistogramWorks) {
    auto& r = MetricsRegistry::instance();
    r.histogram_observe("test_hist", 0.5);
    r.histogram_observe("test_hist", 5.0);
    
    std::string exported = r.export_prometheus();
    EXPECT_TRUE(exported.find("test_hist_sum 5.5") != std::string::npos);
    EXPECT_TRUE(exported.find("test_hist_count 2") != std::string::npos);
    EXPECT_TRUE(exported.find("test_hist_bucket{le=\"1.000000\"} 1") != std::string::npos);
    EXPECT_TRUE(exported.find("test_hist_bucket{le=\"5.000000\"} 2") != std::string::npos);
}
