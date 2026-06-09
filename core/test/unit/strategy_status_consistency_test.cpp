#include "config/Config.hpp"

#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <sstream>
#include <string>

namespace {

std::filesystem::path find_repo_root() {
    auto p = std::filesystem::current_path();
    for (int i = 0; i < 8; ++i) {
        if (std::filesystem::exists(p / "docs/spec/STRATEGIES.md") &&
            std::filesystem::exists(p / "config/config.example.toml")) {
            return p;
        }
        if (!p.has_parent_path())
            break;
        p = p.parent_path();
    }
    return std::filesystem::current_path();
}

std::string read_file(const std::filesystem::path& path) {
    std::ifstream in(path);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

void expect_contains(const std::string& haystack, const std::string& needle) {
    EXPECT_NE(haystack.find(needle), std::string::npos) << "missing: " << needle;
}

} // namespace

TEST(StrategyStatusConsistencyTest, ExampleConfigParsesSafeFlushReversalDefaults) {
    const auto root = find_repo_root();

    trade_bot::Config::load((root / "config/config.example.toml").string());

    EXPECT_FALSE(trade_bot::Config::get<bool>("strategies.flushreversal.allow_live"));
    EXPECT_FALSE(trade_bot::Config::get<bool>("strategies.flushreversal.live_gates_implemented"));
    EXPECT_TRUE(trade_bot::Config::get<bool>("strategies.flushreversal.require_liquidation_flush"));
    EXPECT_TRUE(trade_bot::Config::get<bool>(
        "strategies.flushreversal.require_open_interest_confirmation"));
    EXPECT_DOUBLE_EQ(trade_bot::Config::get<double>("strategies.leaderlag.min_correlation"), 0.6);
}

TEST(StrategyStatusConsistencyTest, SpecsAndConfigKeepLeaderLagAndFlushReversalGates) {
    const auto root = find_repo_root();
    const auto strategies = read_file(root / "docs/spec/STRATEGIES.md");
    const auto signals = read_file(root / "docs/spec/SIGNAL_DETECTION.md");
    const auto tasks = read_file(root / "docs/spec/TASK_SPECS.md");
    const auto config = read_file(root / "config/config.example.toml");

    expect_contains(strategies, "## 3. LeaderLag");
    expect_contains(strategies, "**Status:** `gated`");
    expect_contains(strategies, "Allowed execution modes");
    expect_contains(strategies, "runtime rejects live `FlushReversal` plans unconditionally");
    expect_contains(strategies, "`allow_live=true` alone is insufficient");

    expect_contains(signals, "**Status:** `gated` input for `LeaderLag`");
    expect_contains(signals, "**Status:** `phase-later` live gate for `FlushReversal`");
    expect_contains(signals, "Plain");
    expect_contains(signals, "`TapeFlush` or `FlushNoLiq` must not satisfy live mode");

    expect_contains(tasks, "**Status:** `gated` (FN-004)");
    expect_contains(tasks, "allow_live=true` alone is");
    expect_contains(tasks, "not sufficient for live-grade approval");
    expect_contains(tasks, "Plain `TapeFlush` cannot satisfy live mode");

    expect_contains(config, "[strategies.leaderlag]");
    expect_contains(config, "min_correlation = 0.6");
    expect_contains(config, "lag_max_age_ms = 3000");
    expect_contains(config, "[strategies.flushreversal]");
    expect_contains(config, "allow_live = false");
    expect_contains(config, "live_gates_implemented = false");
    expect_contains(config, "require_liquidation_flush = true");
    expect_contains(config, "require_open_interest_confirmation = true");
}
