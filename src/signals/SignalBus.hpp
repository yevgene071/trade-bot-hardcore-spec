#pragma once

#include "Signal.hpp"
#include <functional>
#include <vector>
#include <mutex>

namespace trade_bot {

/**
 * Single-threaded sync signal bus.
 * Although it's intended to be called from a single thread (FeatureExtractor cadence),
 * we add a mutex for safety if some detectors are async.
 */
class SignalBus {
public:
    using Subscriber = std::function<void(const Signal&)>;

    void subscribe(Subscriber sub);
    void publish(const Signal& signal);

private:
    std::vector<Subscriber> subscribers_;
    std::mutex              mtx_;
};

} // namespace trade_bot
