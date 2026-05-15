#pragma once

#include <chrono>

namespace trade_bot {

/// Pluggable clock abstraction. Allows ReplayFeed to use either system time
/// (live playback / sleep) or virtual time (deterministic unit tests).
class IClock {
public:
    using time_point = std::chrono::system_clock::time_point;

    virtual ~IClock() = default;

    virtual time_point now() const = 0;

    /// Block until `tp` is reached. For VirtualClock this is a no-op (the clock
    /// is advanced explicitly by the caller / driver).
    virtual void sleep_until(time_point tp) = 0;
};

}  // namespace trade_bot
