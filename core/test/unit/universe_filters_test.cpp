#include "universe/UniverseFilters.hpp"
#include "logger/Logger.hpp"

#include <gtest/gtest.h>

using namespace trade_bot;

namespace {

class UniverseFiltersTest : public ::testing::Test {
protected:
    void SetUp() override { Logger::init(); }
};

}  // namespace

TEST_F(UniverseFiltersTest, GlobMatchBasic) {
    EXPECT_TRUE (UniverseFilters::glob_match("*USDT", "BTCUSDT"));
    EXPECT_TRUE (UniverseFilters::glob_match("*USDT", "ETHUSDT"));
    EXPECT_FALSE(UniverseFilters::glob_match("*USDT", "BTCBUSD"));
    EXPECT_TRUE (UniverseFilters::glob_match("*UP*",  "BTCUPUSDT"));
    EXPECT_TRUE (UniverseFilters::glob_match("*UP*",  "UPBTC"));
    EXPECT_FALSE(UniverseFilters::glob_match("*UP*",  "BTCUSDT"));
    EXPECT_TRUE (UniverseFilters::glob_match("1000*", "1000FLOKIUSDT"));
    EXPECT_FALSE(UniverseFilters::glob_match("1000*", "FLOKI1000"));
    EXPECT_TRUE (UniverseFilters::glob_match("BTC*", "BTCUSDT"));
    EXPECT_TRUE (UniverseFilters::glob_match("*",     "anything"));
    EXPECT_TRUE (UniverseFilters::glob_match("BTCUSDT", "BTCUSDT"));
    EXPECT_FALSE(UniverseFilters::glob_match("BTCUSDT", "BTCUSDD"));
}

TEST_F(UniverseFiltersTest, AcceptsAcceptsByDefault) {
    UniverseFilters f;     // empty config = accept everything
    EXPECT_TRUE(f.accepts("ANYTHING"));
}

TEST_F(UniverseFiltersTest, DenyPatternsBlock) {
    UniverseFilters::Config cfg{};
    cfg.deny_patterns = {"*UP*", "*BULL*", "*1000*"};
    UniverseFilters f{cfg};
    EXPECT_FALSE(f.accepts("BTCUPUSDT"));
    EXPECT_FALSE(f.accepts("ETHBULLUSDT"));
    EXPECT_FALSE(f.accepts("1000FLOKIUSDT"));
    EXPECT_TRUE (f.accepts("BTCUSDT"));
}

TEST_F(UniverseFiltersTest, AllowAndDenyCombined) {
    UniverseFilters::Config cfg{};
    cfg.allow_patterns = {"*USDT"};
    cfg.deny_patterns  = {"*UP*"};
    UniverseFilters f{cfg};
    EXPECT_TRUE (f.accepts("BTCUSDT"));
    EXPECT_FALSE(f.accepts("BTCUPUSDT"));   // deny wins over allow
    EXPECT_FALSE(f.accepts("BTCBUSD"));     // not in allow
}

TEST_F(UniverseFiltersTest, ManualAllowDenyOverride) {
    UniverseFilters::Config cfg{};
    cfg.deny_patterns  = {"*"};            // global ban
    cfg.manual_allow   = {"BTCUSDT"};
    cfg.manual_deny    = {"BANNED"};
    UniverseFilters f{cfg};
    EXPECT_TRUE (f.accepts("BTCUSDT"));    // manual allow > deny pattern
    EXPECT_FALSE(f.accepts("BANNED"));     // manual deny is absolute
    EXPECT_FALSE(f.accepts("ETHUSDT"));    // ban-all default
}

TEST_F(UniverseFiltersTest, AcceptsWireAndInternalTickerFormats) {
    UniverseFilters::Config cfg{};
    cfg.allow_patterns = {"*USDT"};
    cfg.manual_deny = {"ETHUSDT"};
    UniverseFilters f{cfg};

    EXPECT_TRUE(f.accepts("BTCUSDT"));
    EXPECT_TRUE(f.accepts("BTC_USDT"));
    EXPECT_FALSE(f.accepts("ETH_USDT"));
}
