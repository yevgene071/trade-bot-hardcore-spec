#include "NewsCalendar.hpp"

#include "logger/Logger.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>

#include <poll.h>
#include <sys/inotify.h>
#include <unistd.h>

namespace trade_bot {

namespace {

// SIGHUP delivery is recorded into this volatile atomic by a portable
// async-signal-safe handler. The watcher thread polls it every 100 ms.
volatile std::sig_atomic_t g_sighup_pending = 0;
std::atomic<bool>          g_sighup_installed{false};

void sighup_handler(int /*sig*/) noexcept {
    g_sighup_pending = 1;
}

std::chrono::system_clock::time_point parse_iso8601(const std::string& s) {
    std::tm tm{};
    std::istringstream ss(s);
    ss >> std::get_time(&tm, "%Y-%m-%dT%H:%M:%S");
    if (ss.fail()) {
        throw std::runtime_error("NewsCalendar: bad ts_utc value: " + s);
    }
#if defined(__linux__)
    const auto secs = timegm(&tm);
#else
    const auto secs = ::mktime(&tm);
#endif
    return std::chrono::system_clock::from_time_t(secs);
}

}  // namespace

void NewsCalendar::install_sighup_handler_() {
    bool expected = false;
    if (!g_sighup_installed.compare_exchange_strong(expected, true)) {
        return;
    }
    struct sigaction sa{};
    sa.sa_handler = sighup_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    ::sigaction(SIGHUP, &sa, nullptr);
}

NewsCalendar::NewsCalendar()
    : events_(std::make_shared<const std::vector<Event>>()) {}

NewsCalendar::~NewsCalendar() {
    stop_watching();
}

std::vector<NewsCalendar::Event> NewsCalendar::parse_(const std::string& path) {
    std::ifstream in(path);
    if (!in) {
        throw std::runtime_error("NewsCalendar: cannot open " + path);
    }
    nlohmann::json j;
    try {
        in >> j;
    } catch (const std::exception& ex) {
        throw std::runtime_error("NewsCalendar: bad JSON in " + path + ": " + ex.what());
    }
    if (!j.is_array()) {
        throw std::runtime_error("NewsCalendar: top-level must be array (" + path + ")");
    }

    std::vector<Event> out;
    out.reserve(j.size());
    for (const auto& item : j) {
        if (!item.is_object()) {
            throw std::runtime_error("NewsCalendar: non-object event entry");
        }
        if (!item.contains("ts_utc") || !item["ts_utc"].is_string()) {
            throw std::runtime_error("NewsCalendar: missing/invalid ts_utc");
        }
        if (!item.contains("importance") || !item["importance"].is_number_integer()) {
            throw std::runtime_error("NewsCalendar: missing/invalid importance");
        }
        Event e;
        e.ts_utc     = parse_iso8601(item["ts_utc"].get<std::string>());
        e.importance = item["importance"].get<int>();
        if (e.importance < 1 || e.importance > 3) {
            throw std::runtime_error("NewsCalendar: importance must be 1..3");
        }
        if (item.contains("ticker") && item["ticker"].is_string()) {
            e.ticker = item["ticker"].get<std::string>();
        }
        if (item.contains("note") && item["note"].is_string()) {
            e.note = item["note"].get<std::string>();
        }
        out.push_back(std::move(e));
    }
    std::sort(out.begin(), out.end(),
              [](const Event& a, const Event& b) { return a.ts_utc < b.ts_utc; });
    return out;
}

void NewsCalendar::swap_in_(std::vector<Event> ev, std::string path) {
    auto snap = std::make_shared<const std::vector<Event>>(std::move(ev));
    std::lock_guard<std::mutex> lk(mtx_);
    events_ = std::move(snap);
    path_   = std::move(path);
}

void NewsCalendar::load(const std::string& path) {
    auto ev = parse_(path);   // throws on failure — old state preserved
    LOG_INFO("NewsCalendar: loaded {} events from {}", ev.size(), path);
    swap_in_(std::move(ev), path);
}

void NewsCalendar::reload() {
    std::string p;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        p = path_;
    }
    if (p.empty()) {
        return;
    }
    try {
        auto ev = parse_(p);
        LOG_INFO("NewsCalendar: reloaded {} events from {}", ev.size(), p);
        swap_in_(std::move(ev), p);
    } catch (const std::exception& ex) {
        LOG_WARN("NewsCalendar: reload failed for {}: {} — keeping previous state",
                 p, ex.what());
    }
}

std::optional<int64_t> NewsCalendar::minutes_to_next_news(
    std::chrono::system_clock::time_point now, const Ticker& ticker) const {
    std::shared_ptr<const std::vector<Event>> snap;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        snap = events_;
    }
    if (!snap || snap->empty()) {
        return std::nullopt;
    }
    for (const auto& e : *snap) {
        if (e.ts_utc < now) continue;
        if (e.ticker.has_value() && *e.ticker != ticker) continue;
        const auto delta = e.ts_utc - now;
        return std::chrono::duration_cast<std::chrono::minutes>(delta).count();
    }
    return std::nullopt;
}

size_t NewsCalendar::size() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return events_ ? events_->size() : 0;
}

std::string NewsCalendar::path() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return path_;
}

void NewsCalendar::start_watching() {
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true)) {
        return;
    }
    install_sighup_handler_();

    inotify_fd_ = ::inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (inotify_fd_ < 0) {
        LOG_WARN("NewsCalendar: inotify_init1 failed: {} — falling back to SIGHUP only",
                 std::strerror(errno));
    } else {
        std::string p;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            p = path_;
        }
        if (!p.empty()) {
            inotify_wd_ = ::inotify_add_watch(
                inotify_fd_, p.c_str(),
                IN_CLOSE_WRITE | IN_MODIFY | IN_MOVE_SELF | IN_DELETE_SELF);
            if (inotify_wd_ < 0) {
                LOG_WARN("NewsCalendar: inotify_add_watch({}) failed: {}",
                         p, std::strerror(errno));
            }
        }
    }

    watcher_ = std::thread([this] { watch_loop_(); });
}

void NewsCalendar::stop_watching() {
    bool expected = true;
    if (!running_.compare_exchange_strong(expected, false)) {
        return;
    }
    if (watcher_.joinable()) {
        watcher_.join();
    }
    if (inotify_wd_ >= 0 && inotify_fd_ >= 0) {
        ::inotify_rm_watch(inotify_fd_, inotify_wd_);
    }
    if (inotify_fd_ >= 0) {
        ::close(inotify_fd_);
    }
    inotify_fd_ = -1;
    inotify_wd_ = -1;
}

void NewsCalendar::watch_loop_() {
    char buf[4096];
    while (running_.load()) {
        if (g_sighup_pending) {
            g_sighup_pending = 0;
            LOG_INFO("NewsCalendar: SIGHUP received → reloading");
            reload();
        }

        if (inotify_fd_ < 0) {
            // No inotify, only poll the SIGHUP flag.
            std::this_thread::sleep_for(std::chrono::milliseconds{100});
            continue;
        }

        struct pollfd pfd{};
        pfd.fd     = inotify_fd_;
        pfd.events = POLLIN;
        const int rc = ::poll(&pfd, 1, /*timeout_ms=*/100);
        if (rc < 0) {
            if (errno == EINTR) continue;
            LOG_WARN("NewsCalendar: poll failed: {}", std::strerror(errno));
            std::this_thread::sleep_for(std::chrono::milliseconds{200});
            continue;
        }
        if (rc == 0) {
            continue;  // timeout; loop to re-check sighup_pending and running_
        }
        if (pfd.revents & POLLIN) {
            const ssize_t n = ::read(inotify_fd_, buf, sizeof(buf));
            if (n <= 0) {
                continue;
            }
            // Drain the entire buffer; any modify/close-write triggers reload.
            bool needs_reload = false;
            ssize_t off = 0;
            while (off < n) {
                const auto* ev = reinterpret_cast<const inotify_event*>(buf + off);
                if (ev->mask & (IN_CLOSE_WRITE | IN_MODIFY | IN_MOVE_SELF | IN_DELETE_SELF)) {
                    needs_reload = true;
                }
                off += static_cast<ssize_t>(sizeof(inotify_event)) + ev->len;
            }
            if (needs_reload) {
                LOG_INFO("NewsCalendar: file change detected → reloading");
                // Re-arm the watch in case the file was replaced via rename.
                std::string p;
                {
                    std::lock_guard<std::mutex> lk(mtx_);
                    p = path_;
                }
                if (!p.empty() && inotify_fd_ >= 0) {
                    if (inotify_wd_ >= 0) ::inotify_rm_watch(inotify_fd_, inotify_wd_);
                    inotify_wd_ = ::inotify_add_watch(
                        inotify_fd_, p.c_str(),
                        IN_CLOSE_WRITE | IN_MODIFY | IN_MOVE_SELF | IN_DELETE_SELF);
                }
                reload();
            }
        }
    }
}

}  // namespace trade_bot
