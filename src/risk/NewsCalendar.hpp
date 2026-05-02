#pragma once

#include "domain/Types.hpp"

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace trade_bot {

/**
 * T0-NEWS: schedule of upcoming macro / ticker-specific news events.
 *
 * The bot consults this calendar to refuse new entries inside `news_blackout`
 * windows (R12) — both in paper and live trading.
 *
 * File format (JSON array):
 *   [
 *     {"ts_utc": "2026-05-15T12:30:00Z", "importance": 3, "note": "FOMC"},
 *     {"ts_utc": "2026-05-16T08:30:00Z", "importance": 2,
 *      "ticker": "BTCUSDT", "note": "BTC ETF inflow report"}
 *   ]
 *
 * Hot-reload triggers (Linux):
 *   - SIGHUP                            → calendar reloaded from `path_`
 *   - inotify on the file (CLOSE_WRITE) → reloaded automatically
 */
class NewsCalendar {
public:
    struct Event {
        std::chrono::system_clock::time_point ts_utc;
        int                                   importance;   // 1..3
        std::optional<Ticker>                 ticker;        // nullopt = applies to every ticker
        std::string                           note;
    };

    NewsCalendar();
    ~NewsCalendar();

    NewsCalendar(const NewsCalendar&)            = delete;
    NewsCalendar& operator=(const NewsCalendar&) = delete;

    /// Parse and validate `path`. Throws std::runtime_error on parse / schema
    /// errors. The previous calendar stays in place if loading fails.
    void load(const std::string& path);

    /// Re-parse from the most recent successful `load()` path.
    void reload();

    /// Spawn the watcher thread (inotify + SIGHUP). Idempotent.
    void start_watching();
    /// Stop the watcher and join the thread.
    void stop_watching();

    /// Minutes to the nearest applicable event for `ticker` after `now`.
    /// An event applies if its `ticker` field is empty OR equals `ticker`.
    /// Returns std::nullopt if there are no future applicable events.
    std::optional<int64_t> minutes_to_next_news(
        std::chrono::system_clock::time_point now,
        const Ticker&                         ticker) const;

    /// Total events currently loaded (for diagnostics / tests).
    size_t size() const;

    /// The file path of the last successful load (or empty).
    std::string path() const;

private:
    void watch_loop_();
    static void install_sighup_handler_();

    std::vector<Event>                   parse_(const std::string& path);
    void                                 swap_in_(std::vector<Event> ev, std::string path);

    mutable std::mutex                   mtx_;
    std::shared_ptr<const std::vector<Event>> events_;   // immutable snapshot
    std::string                          path_;

    std::atomic<bool>                    running_{false};
    std::thread                          watcher_;
    int                                  inotify_fd_{-1};
    int                                  inotify_wd_{-1};
};

}  // namespace trade_bot
