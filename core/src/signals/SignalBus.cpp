#include "SignalBus.hpp"
#include "logger/Logger.hpp"

namespace trade_bot {

SignalBus::SubscriptionId SignalBus::subscribe(Subscriber sub) {
    std::lock_guard<std::mutex> lk(mtx_);
    SubscriptionId id = next_id_++;
    subscribers_.push_back({id, std::move(sub)});
    return id;
}

void SignalBus::unsubscribe(SubscriptionId id) { // AD1
    std::lock_guard<std::mutex> lk(mtx_);
    for (auto it = subscribers_.begin(); it != subscribers_.end(); ++it) {
        if (it->id == id) { subscribers_.erase(it); break; }
    }
}

void SignalBus::publish(const Signal& signal) {
    if (in_publish_) {
        pending_signals_.push_back(signal);
        LOG_DEBUG("SignalBus: queued nested signal kind={} for {} (deferred dispatch)",
                 static_cast<int>(signal.kind), signal.ticker);
        return;
    }

    // Snapshot subscribers under lock to guard against subscribe-during-dispatch
    // (iterator invalidation) and concurrent subscribe() from other threads.
    auto dispatch_one = [this](const Signal& sig) {
        absl::InlinedVector<Entry, 8> subs;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            subs = subscribers_;
        }
        in_publish_ = true;
        try {
            for (const auto& e : subs) e.fn(sig);
        } catch (...) {
            // Exception safety: reset flag and drain pending queue before propagating.
            in_publish_ = false;
            pending_signals_.clear();
            throw;
        }
        in_publish_ = false;
    };

    dispatch_one(signal);

    while (!pending_signals_.empty()) {
        Signal deferred = std::move(pending_signals_.front());
        pending_signals_.pop_front();
        dispatch_one(deferred);
    }
}

} // namespace trade_bot
