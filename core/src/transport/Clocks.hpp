#pragma once

#include "IClock.hpp"

#include <atomic>
#include <thread>

namespace trade_bot {

class WallClock : public IClock {
public:
    time_point now() const override { return std::chrono::system_clock::now(); }
    void sleep_until(time_point tp) override {
        std::this_thread::sleep_until(tp);
    }
};

/// Test clock: starts at a fixed epoch, advances only when the caller asks.
/// `sleep_until` is a no-op (deterministic playback).
class VirtualClock : public IClock {
public:
    explicit VirtualClock(time_point start = time_point{})
        : current_(start.time_since_epoch().count()) {}

    time_point now() const override {
        return time_point{std::chrono::system_clock::duration{current_.load()}};
    }

    void sleep_until(time_point /*tp*/) override { /* no-op */ }

    void set(time_point tp) {
        current_.store(tp.time_since_epoch().count());
    }

    void advance(std::chrono::nanoseconds delta) {
        current_.fetch_add(
            std::chrono::duration_cast<std::chrono::system_clock::duration>(delta).count());
    }

private:
    std::atomic<std::chrono::system_clock::duration::rep> current_;
};

}  // namespace trade_bot
