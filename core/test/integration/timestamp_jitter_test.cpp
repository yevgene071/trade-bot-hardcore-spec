#include "perf/TimestampMonitor.hpp"
#include "transport/BeastWsClient.hpp"
#include "transport/MarketDataFeed.hpp"
#include "transport/MetaScalpDiscovery.hpp"
#include "transport/CurlHttpClient.hpp"
#include "logger/Logger.hpp"

#include <boost/asio/io_context.hpp>
#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <memory>
#include <thread>

using namespace trade_bot;
using namespace std::chrono_literals;

namespace {

constexpr int kTargetPairs = 1000;
constexpr auto kMaxWait = 120s;
constexpr int64_t kThresholdP99Us = 10'000;
constexpr int64_t kThresholdJitterUs = 2'000;

}  // namespace

// T0-MONITOR-TIMESTAMPS integration:
// connect to live MetaScalp, subscribe to BTCUSDT, collect ≥1000 pairs of
// (trade_update, orderbook_update), assert p99 latency < 10 ms and jitter < 2 ms.
//
// Skipped unless WITH_METASCALP=1 is exported.
TEST(TimestampJitter, MetaScalpStreamMeetsThresholds) {
    if (!std::getenv("WITH_METASCALP")) {
        GTEST_SKIP() << "WITH_METASCALP not set";
    }

    Logger::init();

    auto http = std::make_shared<CurlHttpClient>();
    MetaScalpDiscovery discovery{http};
    auto port = discovery.discover();
    if (!port.has_value()) {
        GTEST_SKIP() << "MetaScalp not running on 17845..17855";
    }

    boost::asio::io_context ioc;
    auto ws = std::make_shared<BeastWsClient>(ioc);
    ws->connect("ws://127.0.0.1:" + std::to_string(*port) + "/");

    constexpr int kConnectionId = 1;
    MarketDataFeed feed{ws, kConnectionId};
    TimestampMonitor monitor{};
    feed.add_listener(&monitor);
    feed.start();
    feed.subscribe_ticker("BTCUSDT");

    const auto deadline = std::chrono::steady_clock::now() + kMaxWait;
    while (monitor.sample_count() < kTargetPairs &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(100ms);
    }

    feed.stop();
    ws->disconnect();

    ASSERT_GE(monitor.sample_count(), kTargetPairs)
        << "Collected only " << monitor.sample_count() << " pairs in "
        << std::chrono::duration_cast<std::chrono::seconds>(kMaxWait).count() << "s";

    const auto p99 = monitor.latency_p99_us();
    const auto jitter = monitor.jitter_us();
    LOG_INFO("trade_orderbook_latency_us p50={} p99={} jitter={:.1f} samples={}",
             monitor.latency_p50_us(), p99, jitter, monitor.sample_count());

    if (p99 > kThresholdP99Us) {
        ADD_FAILURE() << "p99=" << p99 << "us > threshold " << kThresholdP99Us << "us";
    }
    if (jitter > static_cast<double>(kThresholdJitterUs)) {
        ADD_FAILURE() << "jitter=" << jitter << "us > threshold " << kThresholdJitterUs << "us";
    }
}
