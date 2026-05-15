#include "metrics/MetricsExporter.hpp"
#include "metrics/MetricsRegistry.hpp"
#include "transport/CurlHttpClient.hpp"
#include <gtest/gtest.h>
#include <thread>

using namespace trade_bot;

TEST(MetricsExporterTest, ExportsCorrectFormat) {
    boost::asio::io_context ioc;
    MetricsExporter exporter(ioc, "127.0.0.1", 9091);
    exporter.start();

    std::thread t([&ioc]() { ioc.run(); });

    MetricsRegistry::instance().gauge_set("exported_gauge", 123.45);

    CurlHttpClient client;
    auto res = client.get("http://127.0.0.1:9091/metrics");
    
    EXPECT_EQ(res.status, 200);
    EXPECT_TRUE(res.body.find("exported_gauge 123.45") != std::string::npos);

    ioc.stop();
    if (t.joinable()) t.join();
}
