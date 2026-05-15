#include "risk/NewsCalendar.hpp"
#include "logger/Logger.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <csignal>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <thread>

using namespace trade_bot;
using namespace std::chrono_literals;

namespace {

class NewsCalendarTest : public ::testing::Test {
protected:
    void SetUp() override {
        Logger::init();
        path_ = std::filesystem::temp_directory_path() /
                ("news_calendar_test_" + std::to_string(::getpid()) + ".json");
    }
    void TearDown() override {
        std::error_code ec;
        std::filesystem::remove(path_, ec);
    }
    void write(const std::string& body) {
        std::ofstream f(path_);
        f << body;
    }
    std::filesystem::path path_;
};

constexpr auto kTimeFormat = "%Y-%m-%dT%H:%M:%SZ";

std::string at_offset_minutes(int minutes_from_now) {
    using namespace std::chrono;
    auto t = system_clock::now() + std::chrono::minutes{minutes_from_now};
    auto tt = system_clock::to_time_t(t);
    std::tm tm{};
    gmtime_r(&tt, &tm);
    char buf[32];
    std::strftime(buf, sizeof(buf), kTimeFormat, &tm);
    return buf;
}

}  // namespace

TEST_F(NewsCalendarTest, LoadValidFile) {
    write(R"([
        {"ts_utc": "2026-05-15T12:30:00Z", "importance": 3, "note": "FOMC"},
        {"ts_utc": "2026-05-16T08:30:00Z", "importance": 2, "ticker": "BTCUSDT"}
    ])");
    NewsCalendar cal;
    cal.load(path_.string());
    EXPECT_EQ(cal.size(), 2u);
}

TEST_F(NewsCalendarTest, RejectsNonArrayJson) {
    write(R"({"not": "an array"})");
    NewsCalendar cal;
    EXPECT_THROW(cal.load(path_.string()), std::runtime_error);
}

TEST_F(NewsCalendarTest, RejectsBadTimestamp) {
    write(R"([{"ts_utc": "yesterday", "importance": 2}])");
    NewsCalendar cal;
    EXPECT_THROW(cal.load(path_.string()), std::runtime_error);
}

TEST_F(NewsCalendarTest, RejectsImportanceOutOfRange) {
    write(R"([{"ts_utc": "2026-05-15T12:00:00Z", "importance": 9}])");
    NewsCalendar cal;
    EXPECT_THROW(cal.load(path_.string()), std::runtime_error);
}

TEST_F(NewsCalendarTest, MinutesToNextNewsRespectsTickerFilter) {
    // T+15 BTC-only, T+30 global
    write("[\n"
          "  {\"ts_utc\": \"" + at_offset_minutes(15) + "\", \"importance\": 2, \"ticker\": \"BTCUSDT\"},\n"
          "  {\"ts_utc\": \"" + at_offset_minutes(30) + "\", \"importance\": 3}\n"
          "]");
    NewsCalendar cal;
    cal.load(path_.string());
    const auto now = std::chrono::system_clock::now();

    auto btc = cal.minutes_to_next_news(now, "BTCUSDT");
    ASSERT_TRUE(btc.has_value());
    EXPECT_GE(*btc, 14);
    EXPECT_LE(*btc, 16);

    auto eth = cal.minutes_to_next_news(now, "ETHUSDT");
    ASSERT_TRUE(eth.has_value());
    EXPECT_GE(*eth, 29);
    EXPECT_LE(*eth, 31);
}

TEST_F(NewsCalendarTest, MinutesToNextNewsIgnoresPastEvents) {
    write("[{\"ts_utc\": \"" + at_offset_minutes(-60) + "\", \"importance\": 1}]");
    NewsCalendar cal;
    cal.load(path_.string());
    EXPECT_EQ(cal.minutes_to_next_news(std::chrono::system_clock::now(), "BTCUSDT"),
              std::nullopt);
}

TEST_F(NewsCalendarTest, ReloadPicksUpNewContent) {
    write(R"([{"ts_utc": "2026-05-15T12:00:00Z", "importance": 2}])");
    NewsCalendar cal;
    cal.load(path_.string());
    EXPECT_EQ(cal.size(), 1u);

    write(R"([
        {"ts_utc": "2026-05-15T12:00:00Z", "importance": 2},
        {"ts_utc": "2026-05-16T12:00:00Z", "importance": 3},
        {"ts_utc": "2026-05-17T12:00:00Z", "importance": 1}
    ])");
    cal.reload();
    EXPECT_EQ(cal.size(), 3u);
}

TEST_F(NewsCalendarTest, ReloadFailureKeepsPreviousState) {
    write(R"([{"ts_utc": "2026-05-15T12:00:00Z", "importance": 2}])");
    NewsCalendar cal;
    cal.load(path_.string());
    EXPECT_EQ(cal.size(), 1u);

    write("garbage");
    cal.reload();              // logs WARN and keeps the old calendar
    EXPECT_EQ(cal.size(), 1u);
}

TEST_F(NewsCalendarTest, HotReloadDetectsFileRewriteWithin2Sec) {
    write(R"([{"ts_utc": "2026-05-15T12:00:00Z", "importance": 2}])");
    NewsCalendar cal;
    cal.load(path_.string());
    cal.start_watching();

    // Rewrite with 3 events
    write(R"([
        {"ts_utc": "2026-05-15T12:00:00Z", "importance": 2},
        {"ts_utc": "2026-05-16T12:00:00Z", "importance": 3},
        {"ts_utc": "2026-05-17T12:00:00Z", "importance": 1}
    ])");

    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (cal.size() != 3 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(50ms);
    }
    cal.stop_watching();
    EXPECT_EQ(cal.size(), 3u);
}

TEST_F(NewsCalendarTest, SighupTriggersReload) {
    write(R"([{"ts_utc": "2026-05-15T12:00:00Z", "importance": 2}])");
    NewsCalendar cal;
    cal.load(path_.string());
    cal.start_watching();
    // Give the watcher thread a moment to install the SIGHUP handler.
    std::this_thread::sleep_for(50ms);

    write(R"([
        {"ts_utc": "2026-05-15T12:00:00Z", "importance": 2},
        {"ts_utc": "2026-05-16T12:00:00Z", "importance": 3}
    ])");

    // Send SIGHUP to ourselves.
    ::raise(SIGHUP);

    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (cal.size() != 2 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(50ms);
    }
    cal.stop_watching();
    EXPECT_EQ(cal.size(), 2u);
}
