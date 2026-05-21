#pragma once

#include "Signal.hpp"
#include <functional>
#include <deque>
#include <mutex>
#include "absl/container/inlined_vector.h"

namespace trade_bot {

/**
 * Single-threaded sync signal bus with deferred nested signal dispatch.
 * 
 * P0 DETERMINISM FIX: Instead of dropping nested signals (when a subscriber
 * calls publish() during signal dispatch), queue them for deferred dispatch
 * after the current dispatch completes. This ensures all signals are processed
 * in a deterministic order without loss.
 */
class SignalBus {
public:
    using Subscriber     = std::function<void(const Signal&)>;
    using SubscriptionId = size_t;

    SubscriptionId subscribe(Subscriber sub);
    void           unsubscribe(SubscriptionId id); // AD1
    void           publish(const Signal& signal);

private:
    struct Entry {
        SubscriptionId id;
        Subscriber     fn;
    };

    // P0-DETERMINISM: InlinedVector avoids heap allocation for typical subscriber count (≤8)
    // and provides stable iteration order. Subscribers register once at startup.
    absl::InlinedVector<Entry, 8> subscribers_;
    mutable std::mutex             mtx_;              // protects subscribers_
    bool                           in_publish_{false};
    std::deque<Signal>             pending_signals_;  // Queue for nested signals
    SubscriptionId                 next_id_{0};
};

} // namespace trade_bot
