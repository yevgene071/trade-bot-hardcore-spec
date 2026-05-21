#include "trading/OrderReconciliator.hpp"
#include "logger/Logger.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <thread>

using namespace trade_bot;
using namespace std::chrono_literals;

namespace {

class OrderReconciliatorTest : public ::testing::Test {
protected:
    void SetUp() override { Logger::init(); }
};

OrderIntent make_intent(int64_t id = 1001, double price = 100.0, double size = 1.0) {
    return OrderIntent{
        /*local_order_id=*/id,
        /*ticker=*/"BTCUSDT",
        /*side=*/Side::Buy,
        /*type=*/OrderType::Limit,
        /*price=*/price,
        /*size=*/size,
    };
}

RestOrder make_server_order(int64_t server_id, double price = 100.0, double size = 1.0,
                            Side side = Side::Buy, OrderType type = OrderType::Limit,
                            const Ticker& ticker = "BTCUSDT") {
    return RestOrder{
        /*id=*/server_id,
        /*client_id=*/std::nullopt,
        /*ticker=*/ticker,
        /*side=*/side,
        /*type=*/type,
        /*price=*/price,
        /*size=*/size,
        /*filled_size=*/0.0,
        /*filled_price=*/0.0,
        /*remaining_size=*/size,
        /*status=*/OrderStatus::Open,
        /*trigger_price=*/std::nullopt,
        /*create_date=*/std::chrono::system_clock::now(),
    };
}

}  // namespace

TEST_F(OrderReconciliatorTest, EnterRegistersIntentOnce) {
    OrderReconciliator r;
    auto intent = make_intent(42);
    auto now = std::chrono::system_clock::now();
    EXPECT_TRUE(r.enter_submit_unknown(intent, now));
    EXPECT_FALSE(r.enter_submit_unknown(intent, now));  // duplicate id
    EXPECT_EQ(r.pending_count(), 1u);
    EXPECT_TRUE(r.has_pending("BTCUSDT"));
    EXPECT_FALSE(r.has_pending("ETHUSDT"));
}

TEST_F(OrderReconciliatorTest, PollWithoutFetcherReturnsEmpty) {
    OrderReconciliator r;
    auto now = std::chrono::system_clock::now();
    r.enter_submit_unknown(make_intent(), now);
    auto results = r.poll_open_orders("BTCUSDT", now);
    EXPECT_TRUE(results.empty());
    // Intent stays pending — poll without fetcher must not silently drop it.
    EXPECT_TRUE(r.has_pending("BTCUSDT"));
}

TEST_F(OrderReconciliatorTest, PollResolvesMatchingOrder) {
    OrderReconciliator r;
    r.set_fetch_open_orders([](const Ticker&) {
        return std::vector<RestOrder>{make_server_order(/*server_id=*/777)};
    });
    auto now = std::chrono::system_clock::now();
    r.enter_submit_unknown(make_intent(42), now);

    auto results = r.poll_open_orders("BTCUSDT", now);
    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(results[0].outcome, ReconcileOutcome::Resolved);
    EXPECT_EQ(results[0].local_order_id, 42);
    ASSERT_TRUE(results[0].server_order_id.has_value());
    EXPECT_EQ(*results[0].server_order_id, 777);
    EXPECT_FALSE(r.has_pending("BTCUSDT"));
}

TEST_F(OrderReconciliatorTest, PollPendingWhenNoMatch) {
    OrderReconciliator r;
    r.set_fetch_open_orders([](const Ticker&) {
        return std::vector<RestOrder>{};  // no orders
    });
    auto now = std::chrono::system_clock::now();
    r.enter_submit_unknown(make_intent(), now);
    auto results = r.poll_open_orders("BTCUSDT", now);
    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(results[0].outcome, ReconcileOutcome::Pending);
    EXPECT_TRUE(r.has_pending("BTCUSDT"));
}

TEST_F(OrderReconciliatorTest, MatchSkipsDifferentTickerSideOrType) {
    OrderReconciliator r;
    r.set_fetch_open_orders([](const Ticker&) {
        std::vector<RestOrder> orders;
        orders.push_back(make_server_order(/*id=*/1, 100.0, 1.0, Side::Sell, OrderType::Limit));
        orders.push_back(make_server_order(/*id=*/2, 100.0, 1.0, Side::Buy, OrderType::Stop));
        orders.push_back(make_server_order(/*id=*/3, 100.0, 1.0, Side::Buy, OrderType::Limit, "ETHUSDT"));
        return orders;
    });
    auto now = std::chrono::system_clock::now();
    r.enter_submit_unknown(make_intent(), now);
    auto results = r.poll_open_orders("BTCUSDT", now);
    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(results[0].outcome, ReconcileOutcome::Pending);  // none matched
}

TEST_F(OrderReconciliatorTest, MatchHonoursPriceAndSizeTolerances) {
    OrderReconciliator::Config cfg{};
    cfg.price_tolerance_bps = 5.0;   // ±0.05%
    cfg.size_tolerance_pct  = 1.0;   // ±1%
    OrderReconciliator r{cfg};

    // 100.0 ± 0.05% = 99.95 .. 100.05
    // 1.0   ± 1%    = 0.99 .. 1.01
    r.set_fetch_open_orders([](const Ticker&) {
        return std::vector<RestOrder>{make_server_order(/*id=*/9, 100.04, 0.991)};
    });
    auto now = std::chrono::system_clock::now();
    r.enter_submit_unknown(make_intent(7), now);

    auto results = r.poll_open_orders("BTCUSDT", now);
    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(results[0].outcome, ReconcileOutcome::Resolved);
    EXPECT_EQ(*results[0].server_order_id, 9);
}

TEST_F(OrderReconciliatorTest, FetchExceptionTreatedAsTransientThenTimeout) {
    OrderReconciliator::Config cfg{};
    cfg.initial_backoff = 1ms;
    cfg.max_backoff     = 1ms;
    cfg.total_timeout   = 50ms;
    OrderReconciliator r{cfg};

    int call_count = 0;
    r.set_fetch_open_orders([&](const Ticker&) -> std::vector<RestOrder> {
        ++call_count;
        throw std::runtime_error("timeout");
    });
    auto now = std::chrono::system_clock::now();
    r.enter_submit_unknown(make_intent(), now);

    // First poll → Pending (after exception).
    auto first = r.poll_open_orders("BTCUSDT", now);
    ASSERT_EQ(first.size(), 1u);
    EXPECT_EQ(first[0].outcome, ReconcileOutcome::Pending);

    // Sleep past total_timeout, poll again → NotFoundTimeout.
    std::this_thread::sleep_for(60ms);
    auto second = r.poll_open_orders("BTCUSDT", std::chrono::system_clock::now());
    ASSERT_EQ(second.size(), 1u);
    EXPECT_EQ(second[0].outcome, ReconcileOutcome::NotFoundTimeout);
    EXPECT_FALSE(r.has_pending("BTCUSDT"));
    EXPECT_GE(call_count, 2);
}

TEST_F(OrderReconciliatorTest, ResolveOrderManualPath) {
    OrderReconciliator r;
    auto now = std::chrono::system_clock::now();
    r.enter_submit_unknown(make_intent(123), now);
    EXPECT_TRUE(r.resolve_order(123, /*server=*/4242));
    EXPECT_FALSE(r.has_pending("BTCUSDT"));
    EXPECT_FALSE(r.resolve_order(123, 9999));  // already gone
}

TEST_F(OrderReconciliatorTest, MultipleIntentsPerTickerPolledTogether) {
    OrderReconciliator r;
    r.set_fetch_open_orders([](const Ticker&) {
        return std::vector<RestOrder>{
            make_server_order(/*id=*/100, 99.0, 1.0),
            make_server_order(/*id=*/200, 101.0, 1.0),
        };
    });
    auto now = std::chrono::system_clock::now();
    r.enter_submit_unknown(make_intent(1, /*price=*/99.0), now);
    r.enter_submit_unknown(make_intent(2, /*price=*/101.0), now);
    r.enter_submit_unknown(make_intent(3, /*price=*/200.0), now);  // no match

    auto results = r.poll_open_orders("BTCUSDT", now);
    ASSERT_EQ(results.size(), 3u);

    int resolved = 0, pending = 0;
    for (const auto& res : results) {
        if (res.outcome == ReconcileOutcome::Resolved)        ++resolved;
        else if (res.outcome == ReconcileOutcome::Pending)    ++pending;
    }
    EXPECT_EQ(resolved, 2);
    EXPECT_EQ(pending, 1);
    EXPECT_TRUE(r.has_pending("BTCUSDT"));
    EXPECT_EQ(r.pending_count(), 1u);
}

TEST_F(OrderReconciliatorTest, BackoffPreventsImmediateRepoll) {
    OrderReconciliator::Config cfg{};
    cfg.initial_backoff = 100ms;
    cfg.max_backoff     = 100ms;
    cfg.total_timeout   = 5'000ms;
    OrderReconciliator r{cfg};

    int call_count = 0;
    r.set_fetch_open_orders([&](const Ticker&) {
        ++call_count;
        return std::vector<RestOrder>{};
    });
    auto now = std::chrono::system_clock::now();
    r.enter_submit_unknown(make_intent(), now);
    r.poll_open_orders("BTCUSDT", now);          // first poll, fetches
    EXPECT_EQ(call_count, 1);

    r.poll_open_orders("BTCUSDT", now);          // immediate repoll skipped due to backoff
    EXPECT_EQ(call_count, 1);
}
