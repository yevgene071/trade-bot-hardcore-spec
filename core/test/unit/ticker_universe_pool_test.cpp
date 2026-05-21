#include "universe/TickerUniverse.hpp"
#include "logger/Logger.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <unordered_map>

using namespace trade_bot;

namespace {

class TickerUniversePoolTest : public ::testing::Test {
protected:
    void SetUp() override { Logger::init(); }
};

}  // namespace

TEST_F(TickerUniversePoolTest, LowVolumeRejected) {
    TickerUniverse::Config cfg{};
    cfg.min_volume_24h_usd = 1'000'000.0;
    cfg.max_avg_spread_bps = 20.0;
    TickerUniverse u{cfg};

    u.set_stats_lookup([](const Ticker& t) -> std::optional<TickerStats> {
        if (t == "OK")   return TickerStats{2'000'000.0, 5.0};
        if (t == "POOR") return TickerStats{  500'000.0, 5.0};
        return std::nullopt;
    });
    u.refresh_pool({"OK", "POOR"}, std::chrono::system_clock::now());
    auto active = u.active();
    EXPECT_EQ(active.size(), 1u);
    EXPECT_EQ(active[0], "OK");
}

TEST_F(TickerUniversePoolTest, HighSpreadRejected) {
    TickerUniverse::Config cfg{};
    cfg.min_volume_24h_usd = 100'000.0;
    cfg.max_avg_spread_bps = 10.0;
    TickerUniverse u{cfg};

    u.set_stats_lookup([](const Ticker& t) -> std::optional<TickerStats> {
        if (t == "OK")   return TickerStats{1'000'000.0, 5.0};
        if (t == "WIDE") return TickerStats{1'000'000.0, 30.0};
        return std::nullopt;
    });
    u.refresh_pool({"OK", "WIDE"}, std::chrono::system_clock::now());
    EXPECT_EQ(u.active(), std::vector<Ticker>{"OK"});
}

TEST_F(TickerUniversePoolTest, ManualAllowForcesIn) {
    TickerUniverse::Config cfg{};
    cfg.filters.deny_patterns = {"*"};       // ban everything by default
    cfg.filters.manual_allow  = {"FORCE"};
    cfg.min_volume_24h_usd = 1.0;
    TickerUniverse u{cfg};

    u.set_stats_lookup([](const Ticker&) -> std::optional<TickerStats> {
        return std::nullopt;                  // no stats — manual_allow bypasses
    });
    u.refresh_pool({"FORCE", "BANNED"}, std::chrono::system_clock::now());
    EXPECT_EQ(u.active(), std::vector<Ticker>{"FORCE"});
}

TEST_F(TickerUniversePoolTest, ManualDenyForcesOut) {
    TickerUniverse::Config cfg{};
    cfg.filters.manual_deny = {"BLOCK"};
    cfg.min_volume_24h_usd  = 1.0;
    cfg.max_avg_spread_bps  = 100.0;
    TickerUniverse u{cfg};
    u.set_stats_lookup([](const Ticker&) {
        return std::optional<TickerStats>{TickerStats{1'000'000.0, 5.0}};
    });
    u.refresh_pool({"BLOCK", "OK"}, std::chrono::system_clock::now());
    EXPECT_EQ(u.active(), std::vector<Ticker>{"OK"});
}

TEST_F(TickerUniversePoolTest, MaxPoolSizeKeepsTopByVolume) {
    TickerUniverse::Config cfg{};
    cfg.max_pool_size      = 3;
    cfg.min_volume_24h_usd = 1.0;
    cfg.max_avg_spread_bps = 100.0;
    TickerUniverse u{cfg};

    std::unordered_map<Ticker, double> volumes = {
        {"A", 5e6}, {"B", 2e6}, {"C", 9e6}, {"D", 7e6}, {"E", 1e6}};
    u.set_stats_lookup([&](const Ticker& t) -> std::optional<TickerStats> {
        auto it = volumes.find(t);
        if (it == volumes.end()) return std::nullopt;
        return TickerStats{it->second, 5.0};
    });
    u.refresh_pool({"A", "B", "C", "D", "E"}, std::chrono::system_clock::now());

    auto active = u.active();
    ASSERT_EQ(active.size(), 3u);
    EXPECT_EQ(active[0], "C");   // 9e6
    EXPECT_EQ(active[1], "D");   // 7e6
    EXPECT_EQ(active[2], "A");   // 5e6
}

TEST_F(TickerUniversePoolTest, TickerMetaCache) {
    TickerUniverse u;
    EXPECT_FALSE(u.meta("BTCUSDT").has_value());
    u.cache_meta("BTCUSDT", TickerMeta{0.01, 1e-5, 0.001, 1000.0});
    auto m = u.meta("BTCUSDT");
    ASSERT_TRUE(m.has_value());
    EXPECT_DOUBLE_EQ(m->price_increment, 0.01);
    EXPECT_DOUBLE_EQ(m->size_increment, 1e-5);
}
