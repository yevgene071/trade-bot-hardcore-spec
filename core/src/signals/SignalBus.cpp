#include "SignalBus.hpp"
#include "logger/Logger.hpp"

namespace trade_bot {

void SignalBus::subscribe(Subscriber sub) {
    subscribers_.push_back(std::move(sub));
}

void SignalBus::publish(const Signal& signal) {
    // B9-FIX: Warn when reentrant publish drops a signal (nested signal emit)
    if (in_publish_) {
        LOG_WARN("SignalBus: dropping nested signal kind={} for {} — reentrant publish suppressed",
                 static_cast<int>(signal.kind), signal.ticker);
        return;
    }
    in_publish_ = true;
    for (const auto& sub : subscribers_) sub(signal);
    in_publish_ = false;
}

} // namespace trade_bot
