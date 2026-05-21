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

TEST_F(OrderBookTest, DeleteEmptyLevelsOnZeroSize) {
    auto ob = make();
    ob.apply_snapshot(make_snapshot());

    // Initial state: 3 bids, 3 asks
    EXPECT_EQ(ob.bid_levels(), 3u);
    EXPECT_EQ(ob.ask_levels(), 3u);

    // Delete one bid and one ask with size=0
    ob.apply_update(make_update({
        {99.99, 0.0, Side::Buy},
        {100.01, 0.0, Side::Sell}
    }));

    EXPECT_EQ(ob.bid_levels(), 2u);
    EXPECT_EQ(ob.ask_levels(), 2u);
    EXPECT_DOUBLE_EQ(*ob.best_bid(), 99.98);
    EXPECT_DOUBLE_EQ(*ob.best_ask(), 100.02);
}

TEST_F(OrderBookTest, GetTopLevelsReturnsCorrectOrderAndCount) {
    auto ob = make();

    // Build a book with 25 levels each side
    OrderBookSnapshot snap{};
    snap.ticker = "BTCUSDT";
    for (int i = 0; i < 25; ++i) {
        snap.bids.push_back({100.00 - 0.01 * i, 1.0 + 0.1 * i, Side::Buy});
        snap.asks.push_back({100.00 + 0.01 * i, 1.0 + 0.1 * i, Side::Sell});
    }
    snap.ts = std::chrono::system_clock::now();
    ob.apply_snapshot(snap);

    // get_top_levels(20) — DS-03 criterion
    auto [bids, asks] = ob.get_top_levels(20);
    EXPECT_EQ(bids.size(), 20u);
    EXPECT_EQ(asks.size(), 20u);

    // Bids must be in descending price order (best first)
    for (std::size_t i = 1; i < bids.size(); ++i) {
        EXPECT_GT(bids[i - 1].price, bids[i].price)
            << "Bids must be descending: " << bids[i-1].price << " > " << bids[i].price;
    }
    // First bid is highest (best)
    EXPECT_DOUBLE_EQ(bids[0].price, 100.00);
    EXPECT_DOUBLE_EQ(bids[0].size, 1.0);

    // Asks must be in ascending price order (best first)
    for (std::size_t i = 1; i < asks.size(); ++i) {
        EXPECT_LT(asks[i - 1].price, asks[i].price)
            << "Asks must be ascending: " << asks[i-1].price << " < " << asks[i].price;
    }
    // First ask is lowest (best)
    EXPECT_DOUBLE_EQ(asks[0].price, 100.00);
    EXPECT_DOUBLE_EQ(asks[0].size, 1.0);
}

TEST_F(OrderBookTest, GetTopLevelsOnSmallBookReturnsAllLevels) {
    auto ob = make();
    ob.apply_snapshot(make_snapshot());

    // Only 3 levels per side — asking for 20 should return all 3
    auto [bids, asks] = ob.get_top_levels(20);
    EXPECT_EQ(bids.size(), 3u);
    EXPECT_EQ(asks.size(), 3u);

    // Verify bid order
    EXPECT_DOUBLE_EQ(bids[0].price, 99.99);
    EXPECT_DOUBLE_EQ(bids[1].price, 99.98);
    EXPECT_DOUBLE_EQ(bids[2].price, 99.97);

    // Verify ask order
    EXPECT_DOUBLE_EQ(asks[0].price, 100.01);
    EXPECT_DOUBLE_EQ(asks[1].price, 100.02);
    EXPECT_DOUBLE_EQ(asks[2].price, 100.03);
}

TEST_F(OrderBookTest, GetTopLevelsZeroReturnsEmpty) {
    auto ob = make();
    ob.apply_snapshot(make_snapshot());

    auto [bids, asks] = ob.get_top_levels(0);
    EXPECT_TRUE(bids.empty());
    EXPECT_TRUE(asks.empty());
}

TEST_F(OrderBookTest, GetTopLevelsNegativeReturnsEmpty) {
    auto ob = make();
    ob.apply_snapshot(make_snapshot());

    auto [bids, asks] = ob.get_top_levels(-5);
    EXPECT_TRUE(bids.empty());
    EXPECT_TRUE(asks.empty());
}

TEST_F(OrderBookTest, IsConsistentDetectsMissingUpdates) {
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

TEST_F(OrderBookTest, ApplyUpdateBatchParallelCrossed) {
    auto ob = make();
    OrderBookSnapshot snap{};
    snap.ticker = "BTCUSDT";
    snap.bids = {{99.00, 1.0, Side::Buy}};
    snap.asks = {{101.00, 1.0, Side::Sell}};
    ob.apply_snapshot(snap);

    // Создаем батч размером 64, чтобы сработала параллельная ветка
    std::vector<PriceLevel> batch;
    // Новый бид пересекает старый аск на 101.00
    batch.push_back({101.50, 1.0, Side::Buy});
    
    // Добавляем 63 фиктивных изменения бидов на низких ценах
    for (int i = 0; i < 63; ++i) {
        batch.push_back({90.00 - i * 0.1, 1.0, Side::Buy});
    }

    ob.apply_update_batch(batch);

    // Проверяем, что старый ask на 101.00 был удален
    auto bb = ob.best_bid();
    auto ba = ob.best_ask();
    ASSERT_TRUE(bb.has_value());
    EXPECT_DOUBLE_EQ(*bb, 101.50);
    EXPECT_FALSE(ba.has_value()); // Старый аск 101.00 удален, книга пуста с этой стороны
}

