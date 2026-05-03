#include "control/DashboardServer.hpp"
#include "logger/Logger.hpp"

#include <boost/asio/io_context.hpp>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <thread>

using namespace trade_bot;

namespace {

class DashboardServerTest : public ::testing::Test {
protected:
    void SetUp() override { Logger::init(); }

    // Find a free TCP port; tests run in parallel via ctest -j.
    static uint16_t pick_port() {
        boost::asio::io_context ioc;
        boost::asio::ip::tcp::acceptor acc(
            ioc, {boost::asio::ip::make_address("127.0.0.1"), 0});
        return acc.local_endpoint().port();
    }
};

DashboardServer::State make_state(double equity = 12345.67) {
    DashboardServer::State s;
    s.version       = "0.0.1";
    s.kill_switch_active = false;
    s.account.equity_usd            = equity;
    s.account.realized_pnl_today_usd = -50.0;
    s.account.unrealized_pnl_usd    = 12.5;
    s.account.free_balance_usd      = 11000.0;
    s.signal_counts["TapeBurst"]    = 7;
    return s;
}

}  // namespace

// #120 — update_state and serialize_state must NOT deadlock when called
// from the same thread that previously held the lock. Pre-fix, this would
// deadlock if a Session was registered and update_state called send_update
// re-entering the mutex.
TEST_F(DashboardServerTest, UpdateStateAndSerializeAreReentrancySafe) {
    boost::asio::io_context ioc;
    DashboardServer ds(ioc, "127.0.0.1", pick_port());

    std::atomic<bool> done{false};
    std::thread t([&] {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
        for (int i = 0; i < 1000; ++i) {
            ds.update_state(make_state(1000.0 + i));
            (void)ds.serialize_state();
            if (std::chrono::steady_clock::now() > deadline) break;
        }
        done.store(true);
    });
    t.join();
    EXPECT_TRUE(done.load()) << "broadcaster looked stuck — deadlock regression";
}

TEST_F(DashboardServerTest, SerializeStatePopulatesAllSections) {
    boost::asio::io_context ioc;
    DashboardServer ds(ioc, "127.0.0.1", pick_port());
    ds.update_state(make_state());

    auto j = nlohmann::json::parse(ds.serialize_state());
    EXPECT_EQ(j["version"],            "0.0.1");
    EXPECT_DOUBLE_EQ(j["account"]["equity_usd"].get<double>(),   12345.67);
    EXPECT_DOUBLE_EQ(j["account"]["realized_pnl_today_usd"].get<double>(), -50.0);
    EXPECT_FALSE(j["kill_switch_active"].get<bool>());
    EXPECT_EQ(j["signal_counts"]["TapeBurst"].get<int>(),        7);
    EXPECT_TRUE(j["open_trades"].is_array());
    EXPECT_TRUE(j["recent_journal"].is_array());
}

// #121 — auth check. Empty token = auth disabled (loopback fallback).
TEST_F(DashboardServerTest, AuthorizeWithoutTokenAcceptsAnything) {
    boost::asio::io_context ioc;
    DashboardServer ds(ioc, "127.0.0.1", pick_port(), /*auth_token=*/"");
    EXPECT_TRUE(ds.authorize(""));
    EXPECT_TRUE(ds.authorize("Bearer anything"));
}

TEST_F(DashboardServerTest, AuthorizeWithTokenRejectsMissingHeader) {
    boost::asio::io_context ioc;
    DashboardServer ds(ioc, "127.0.0.1", pick_port(), /*auth_token=*/"super-secret");
    EXPECT_FALSE(ds.authorize(""));
    EXPECT_FALSE(ds.authorize("Bearer"));            // no token
    EXPECT_FALSE(ds.authorize("Basic super-secret")); // wrong scheme
}

TEST_F(DashboardServerTest, AuthorizeWithTokenAcceptsExactMatchOnly) {
    boost::asio::io_context ioc;
    DashboardServer ds(ioc, "127.0.0.1", pick_port(), /*auth_token=*/"super-secret");
    EXPECT_TRUE (ds.authorize("Bearer super-secret"));
    EXPECT_FALSE(ds.authorize("Bearer super-secret-extra"));   // length differs
    EXPECT_FALSE(ds.authorize("Bearer super-secre"));          // length differs
    EXPECT_FALSE(ds.authorize("Bearer SUPER-SECRET"));         // case-sensitive
    EXPECT_FALSE(ds.authorize("Bearer "));
}

TEST_F(DashboardServerTest, NoSessionsAtConstruction) {
    boost::asio::io_context ioc;
    DashboardServer ds(ioc, "127.0.0.1", pick_port());
    EXPECT_EQ(ds.session_count(), 0u);
}
