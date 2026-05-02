#include "marketdata/OrderBook.hpp"
#include "logger/Logger.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <random>
#include <sys/resource.h>
#include <vector>

using namespace trade_bot;

namespace {

class OrderBookTest : public ::testing::Test {
protected:
    void SetUp() override { Logger::init(); }
    OrderBook make() { return OrderBook{"BTCUSDT", 0.01, 1e-6}; }
};

OrderBookSnapshot make_snapshot() {
    OrderBookSnapshot s{};
    s.ticker = "BTCUSDT";
    s.bids = {{99.99, 1.5, Side::Buy}, {99.98, 2.0, Side::Buy}, {99.97, 3.0, Side::Buy}};
    s.asks = {{100.01, 1.0, Side::Sell}, {100.02, 2.5, Side::Sell}, {100.03, 4.0, Side::Sell}};
    s.ts = std::chrono::system_clock::now();
    return s;
}

OrderBookUpdate make_update(std::vector<PriceLevel> changes) {
    OrderBookUpdate u{};
    u.ticker = "BTCUSDT";
    u.changes = std::move(changes);
    u.ts = std::chrono::system_clock::now();
    return u;
}

long resident_kb() {
    rusage r{};
    getrusage(RUSAGE_SELF, &r);
    return r.ru_maxrss;  // Linux: kilobytes
}

}  // namespace

TEST_F(OrderBookTest, ApplySnapshotPopulatesTopOfBook) {
    auto ob = make();
    ob.apply_snapshot(make_snapshot());

    ASSERT_TRUE(ob.best_bid().has_value());
    ASSERT_TRUE(ob.best_ask().has_value());
    EXPECT_DOUBLE_EQ(*ob.best_bid(), 99.99);
    EXPECT_DOUBLE_EQ(*ob.best_ask(), 100.01);
    EXPECT_NEAR(*ob.spread(), 0.02, 1e-9);
    EXPECT_NEAR(*ob.mid(),    100.0, 1e-9);
    EXPECT_EQ(ob.bid_levels(), 3u);
    EXPECT_EQ(ob.ask_levels(), 3u);
}

TEST_F(OrderBookTest, ApplyUpdateAddsModifiesAndRemovesLevels) {
    auto ob = make();
    ob.apply_snapshot(make_snapshot());

    // add a new bid above all existing (becomes new best)
    ob.apply_update(make_update({{100.00, 0.5, Side::Buy}}));
    EXPECT_DOUBLE_EQ(*ob.best_bid(), 100.00);

    // modify existing ask
    ob.apply_update(make_update({{100.01, 0.25, Side::Sell}}));
    EXPECT_DOUBLE_EQ(*ob.best_ask(), 100.01);
    EXPECT_NEAR(ob.ask_depth(1), 0.25, 1e-9);

    // remove the new top bid (size=0)
    ob.apply_update(make_update({{100.00, 0.0, Side::Buy}}));
    EXPECT_DOUBLE_EQ(*ob.best_bid(), 99.99);
}

TEST_F(OrderBookTest, DepthSumsAcrossLevels) {
    auto ob = make();
    ob.apply_snapshot(make_snapshot());
    EXPECT_NEAR(ob.bid_depth(3), 1.5 + 2.0 + 3.0, 1e-9);
    EXPECT_NEAR(ob.ask_depth(3), 1.0 + 2.5 + 4.0, 1e-9);
    EXPECT_NEAR(ob.bid_depth(2), 1.5 + 2.0, 1e-9);
    EXPECT_NEAR(ob.bid_depth(0), 0.0, 1e-9);
}

TEST_F(OrderBookTest, VolumeAtRangeInclusive) {
    auto ob = make();
    ob.apply_snapshot(make_snapshot());
    EXPECT_NEAR(ob.volume_at_range(99.98, 100.02), 2.0 + 1.5 + 1.0 + 2.5, 1e-9);
    EXPECT_NEAR(ob.volume_at_range(100.0, 100.05), 1.0 + 2.5 + 4.0, 1e-9);
}

TEST_F(OrderBookTest, EmptyBookYieldsNullopt) {
    auto ob = make();
    EXPECT_FALSE(ob.best_bid().has_value());
    EXPECT_FALSE(ob.best_ask().has_value());
    EXPECT_FALSE(ob.spread().has_value());
    EXPECT_FALSE(ob.mid().has_value());
}

TEST_F(OrderBookTest, PriceTickRoundTripIsExact) {
    constexpr double inc = 0.01;
    for (double p = 100.00; p <= 110.00 + 1e-9; p += inc) {
        const auto t = PriceTick::from_price(p, inc);
        EXPECT_NEAR(t.to_price(inc), p, 1e-9) << "p=" << p;
    }
}

TEST_F(OrderBookTest, OneHundredKRandomUpdatesNoMemoryGrowth) {
    auto ob = make();
    ob.apply_snapshot(make_snapshot());

    std::mt19937_64 rng{42};
    std::uniform_real_distribution<double> price(99.50, 100.50);
    std::uniform_real_distribution<double> size(0.0, 5.0);   // 0 → delete
    std::uniform_int_distribution<int>     side(0, 1);

    const long rss_before = resident_kb();

    constexpr int kIters = 100'000;
    for (int i = 0; i < kIters; ++i) {
        const double p   = std::round(price(rng) * 100.0) / 100.0;
        const double sz  = size(rng) < 0.1 ? 0.0 : size(rng);
        const Side   sd  = side(rng) == 0 ? Side::Buy : Side::Sell;
        ob.apply_update(make_update({{p, sz, sd}}));
    }

    const long rss_after = resident_kb();
    EXPECT_LT(rss_after, rss_before * 2 + 50'000)  // 50 MB headroom
        << "rss_before=" << rss_before << " rss_after=" << rss_after;
    EXPECT_EQ(ob.update_count(), kIters + 1);
}

TEST_F(OrderBookTest, ApplyUpdateBatchWorks) {
    auto ob = make();
    ob.apply_snapshot(make_snapshot());

    std::vector<PriceLevel> batch = {
        {100.05, 1.0, Side::Sell},
        {100.06, 1.0, Side::Sell},
        {100.07, 1.0, Side::Sell},
        {100.08, 1.0, Side::Sell},
        {99.90,  1.0, Side::Buy},
        {99.89,  1.0, Side::Buy},
        {99.88,  1.0, Side::Buy},
        {99.87,  1.0, Side::Buy}
    };
    ob.apply_update_batch(batch);
    EXPECT_EQ(ob.bid_levels(), 3 + 4);
    EXPECT_EQ(ob.ask_levels(), 3 + 4);
}

TEST_F(OrderBookTest, SanityCheckDetectsGap) {
    auto ob = make();
    auto snap = make_snapshot();
    ob.apply_snapshot(snap);

    // Snapshot is consistent with itself
    EXPECT_TRUE(ob.is_consistent(snap));

    // Simulate 4 missing updates (more than max_diff=3)
    OrderBookSnapshot corrupted_snap = snap;
    corrupted_snap.bids.push_back({99.00, 1.0, Side::Buy});
    corrupted_snap.bids.push_back({98.00, 1.0, Side::Buy});
    corrupted_snap.bids.push_back({97.00, 1.0, Side::Buy});
    corrupted_snap.bids.push_back({96.00, 1.0, Side::Buy});

    EXPECT_FALSE(ob.is_consistent(corrupted_snap, 3));
    
    // With max_diff=5 it should pass
    EXPECT_TRUE(ob.is_consistent(corrupted_snap, 5));
}
