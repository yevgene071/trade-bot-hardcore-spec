#include "SignalBus.hpp"

namespace trade_bot {

void SignalBus::subscribe(Subscriber sub) {
    std::lock_guard<std::mutex> lk(mtx_);
    subscribers_.push_back(std::move(sub));
}

void SignalBus::publish(const Signal& signal) {
    std::lock_guard<std::mutex> lk(mtx_);
    for (const auto& sub : subscribers_) {
        sub(signal);
    }
}

} // namespace trade_bot
