#include "config/Config.hpp"
#include "logger/Logger.hpp"

#include <fstream>
#include <gtest/gtest.h>

using namespace trade_bot;

class ConfigTest : public ::testing::Test {
  protected:
    void SetUp() override {
        trade_bot::Logger::init("test_logs/config_test.log");
        std::ofstream f("test_config.toml");
        f << "[app]\n"
          << "version = \"0.0.1\"\n"
          << "name = \"test_bot\"\n"
          << "[logger]\n"
          << "level = \"debug\"\n"
          << "path = \"test.log\"\n"
          << "[network]\n"
          << "http_timeout_ms = 5000\n"
          << "[trading]\n"
          << "symbols = [\"BTCUSDT\", \"ETHUSDT\"]\n"
          << "[risk]\n"
          << "max_daily_loss_pct = 3.0\n"
          << "max_per_trade_risk_pct = 0.5\n"
          << "max_concurrent_positions = 3\n"
          << "max_leverage = 5.0\n"
          << "[strategies.leaderlag]\n"
          << "min_correlation = 0.6\n"
          << "max_spread_bps = 3.0\n"
          << "lag_max_age_ms = 3000\n"
          << "max_our_change_2s_pct = 0.1\n"
          << "density_on_path_search_bps = 25.0\n"
          << "[strategies.flushreversal]\n"
          << "allow_live = false\n"
          << "live_gates_implemented = false\n"
          << "require_liquidation_flush = true\n"
          << "require_open_interest_confirmation = true\n"
          << "[section]\n"
          << "key = \"value\"\n"
          << "num = 42\n"
          << "pi = 3.14\n"
          << "flag = true\n"
          << "arr = [1, 2, 3]\n"
          << "float_arr = [0.35, 0.20, 0.12]\n";
        f.close();
    }

    void TearDown() override {
        std::remove("test_config.toml");
    }
};

TEST_F(ConfigTest, LoadAndGet) {
    Config::load("test_config.toml");

    EXPECT_EQ(Config::get<std::string>("section.key"), "value");
    EXPECT_EQ(Config::get<int64_t>("section.num"), 42);
    EXPECT_DOUBLE_EQ(Config::get<double>("section.pi"), 3.14);
    EXPECT_TRUE(Config::get<bool>("section.flag"));
}

TEST_F(ConfigTest, GetVector) {
    Config::load("test_config.toml");
    auto symbols = Config::get<std::vector<std::string>>("trading.symbols");
    ASSERT_EQ(symbols.size(), 2);
    EXPECT_EQ(symbols[0], "BTCUSDT");
    EXPECT_EQ(symbols[1], "ETHUSDT");
}

TEST_F(ConfigTest, GetOr) {
    Config::load("test_config.toml");

    EXPECT_EQ(Config::get_or<int64_t>("section.missing", 100), 100);
    EXPECT_EQ(Config::get_or<std::string>("section.key", "default"), "value");
}

TEST_F(ConfigTest, Has) {
    Config::load("test_config.toml");
    EXPECT_TRUE(Config::has("app.version"));
    EXPECT_FALSE(Config::has("app.missing"));
}

TEST_F(ConfigTest, MissingFile) {
    EXPECT_THROW(Config::load("non_existent.toml"), ConfigError);
}

TEST_F(ConfigTest, WrongType) {
    Config::load("test_config.toml");
    EXPECT_THROW(Config::get<int64_t>("section.key"), ConfigError);
}

TEST_F(ConfigTest, ValidationFail) {
    std::ofstream f("invalid_config.toml");
    f << "[app]\n"
      << "version = \"0.0.1\"\n"; // Missing many required keys
    f.close();

    EXPECT_THROW(Config::load("invalid_config.toml"), ConfigError);
    std::remove("invalid_config.toml");
}

TEST_F(ConfigTest, GetDoubleVector) {
    Config::load("test_config.toml");
    auto values = Config::get<std::vector<double>>("section.float_arr");
    ASSERT_EQ(values.size(), 3);
    EXPECT_DOUBLE_EQ(values[0], 0.35);
    EXPECT_DOUBLE_EQ(values[1], 0.20);
    EXPECT_DOUBLE_EQ(values[2], 0.12);
}

TEST_F(ConfigTest, StrategyStatusGateDefaults) {
    Config::load("test_config.toml");

    EXPECT_DOUBLE_EQ(Config::get<double>("strategies.leaderlag.min_correlation"), 0.6);
    EXPECT_DOUBLE_EQ(Config::get<double>("strategies.leaderlag.max_spread_bps"), 3.0);
    EXPECT_EQ(Config::get<int64_t>("strategies.leaderlag.lag_max_age_ms"), 3000);
    EXPECT_DOUBLE_EQ(Config::get<double>("strategies.leaderlag.max_our_change_2s_pct"), 0.1);
    EXPECT_DOUBLE_EQ(Config::get<double>("strategies.leaderlag.density_on_path_search_bps"), 25.0);

    EXPECT_FALSE(Config::get<bool>("strategies.flushreversal.allow_live"));
    EXPECT_FALSE(Config::get<bool>("strategies.flushreversal.live_gates_implemented"));
    EXPECT_TRUE(Config::get<bool>("strategies.flushreversal.require_liquidation_flush"));
    EXPECT_TRUE(Config::get<bool>("strategies.flushreversal.require_open_interest_confirmation"));
}
