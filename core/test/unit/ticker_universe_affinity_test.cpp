#include "universe/TickerUniverse.hpp"
#include "logger/Logger.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <unordered_map>

using namespace trade_bot;

namespace {

class TickerUniverseAffinityTest : public ::testing::Test {
protected:
    void SetUp() override { Logger::init(); }

    std::unique_ptr<TickerUniverse> make_pool(std::vector<Ticker> tickers) {
        TickerUniverse::Config cfg{};
        cfg.min_volume_24h_usd = 1.0;
        cfg.max_avg_spread_bps = 1000.0;
        auto u = std::make_unique<TickerUniverse>(cfg);
        u->set_stats_lookup([](const Ticker&) {
            return std::optional<TickerStats>{TickerStats{1e6, 5.0}};
        });
        u->refresh_pool(tickers, std::chrono::system_clock::now());
        return u;
    }
};

}  // namespace

TEST_F(TickerUniverseAffinityTest, BounceFromDensityEnabledAtThreshold) {
    auto u = make_pool({"BTCUSDT", "ETHUSDT", "DOGEUSDT"});
    constexpr int min_events_per_hour = 4;
    std::unordered_map<Ticker, int> events_per_hour = {
        {"BTC_USDT", 5}, {"ETH_USDT", 4}, {"DOGE_USDT", 2}};

    u->register_strategy("BounceFromDensity", [&](const Ticker& t) {
        auto it = events_per_hour.find(t);
        return it != events_per_hour.end() && it->second >= min_events_per_hour;
    });
    u->refresh_affinity();

    EXPECT_TRUE (u->is_strategy_enabled("BTCUSDT", "BounceFromDensity"));
    EXPECT_TRUE (u->is_strategy_enabled("ETHUSDT", "BounceFromDensity"));
    EXPECT_FALSE(u->is_strategy_enabled("DOGEUSDT", "BounceFromDensity"));
}

TEST_F(TickerUniverseAffinityTest, LeaderLagRespectsCorrelationAndSelfExclude) {
    auto u = make_pool({"BTCUSDT", "ETHUSDT", "SOLUSDT"});
    const Ticker leader = "BTC_USDT";
    constexpr double min_corr = 0.6;
    const bool exclude_self_for_leader = true;

    std::unordered_map<Ticker, double> corr_60s = {
        {"BTC_USDT", 1.0}, {"ETH_USDT", 0.7}, {"SOL_USDT", 0.4}};

    u->register_strategy("LeaderLag", [&](const Ticker& t) {
        if (exclude_self_for_leader && t == leader) return false;
        auto it = corr_60s.find(t);
        return it != corr_60s.end() && it->second >= min_corr;
    });
    u->refresh_affinity();

    EXPECT_FALSE(u->is_strategy_enabled("BTCUSDT", "LeaderLag"));   // self-excluded
    EXPECT_TRUE (u->is_strategy_enabled("ETHUSDT", "LeaderLag"));   // 0.7 >= 0.6
    EXPECT_FALSE(u->is_strategy_enabled("SOLUSDT", "LeaderLag"));   // 0.4 < 0.6
}

TEST_F(TickerUniverseAffinityTest, BoostDoesNotChangeAffinityUntilRefresh) {
    auto u = make_pool({"BTCUSDT"});
    bool toggle = false;
    u->register_strategy("Strat", [&](const Ticker&) { return toggle; });
    u->refresh_affinity();
    EXPECT_FALSE(u->is_strategy_enabled("BTCUSDT", "Strat"));

    // Boost the ticker — score function is *not* called automatically.
    auto now = std::chrono::system_clock::now();
    u->on_big_event("BTCUSDT", now);
    toggle = true;                                                  // pretend score now would fire
    EXPECT_TRUE(u->is_boosted("BTCUSDT", now));
    EXPECT_FALSE(u->is_strategy_enabled("BTCUSDT", "Strat"))
        << "boost must NOT change affinity state until refresh_affinity()";

    u->refresh_affinity();
    EXPECT_TRUE(u->is_strategy_enabled("BTCUSDT", "Strat"));
}

TEST_F(TickerUniverseAffinityTest, BoostExpiresAfterTtl) {
    TickerUniverse::Config cfg{};
    cfg.boost_ttl = std::chrono::seconds{1};
    cfg.min_volume_24h_usd = 1.0;
    cfg.max_avg_spread_bps = 1000.0;
    TickerUniverse u{cfg};
    u.set_stats_lookup([](const Ticker&) {
        return std::optional<TickerStats>{TickerStats{1e6, 5.0}};
    });
    u.refresh_pool({"BTCUSDT"}, std::chrono::system_clock::now());

    auto t0 = std::chrono::system_clock::now();
    u.on_big_event("BTCUSDT", t0);
    EXPECT_TRUE (u.is_boosted("BTCUSDT", t0));
    EXPECT_TRUE (u.is_boosted("BTCUSDT", t0 + std::chrono::milliseconds{500}));
    EXPECT_FALSE(u.is_boosted("BTCUSDT", t0 + std::chrono::seconds{2}));
}
