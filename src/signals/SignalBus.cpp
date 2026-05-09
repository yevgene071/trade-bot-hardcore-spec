#include "SignalBus.hpp"

namespace trade_bot {

void SignalBus::subscribe(Subscriber sub) {
    subscribers_.push_back(std::move(sub));
}

void SignalBus::publish(const Signal& signal) {
    if (in_publish_) return;
    in_publish_ = true;
    for (const auto& sub : subscribers_) sub(signal);
    in_publish_ = false;
}

} // namespace trade_bot
