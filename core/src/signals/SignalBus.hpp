#pragma once

#include "Signal.hpp"
#include <functional>
#include <vector>

namespace trade_bot {

/**
 * Single-threaded sync signal bus.
 * Reentrancy guard prevents deadlock if a subscriber calls publish().
 */
class SignalBus {
public:
    using Subscriber = std::function<void(const Signal&)>;

    void subscribe(Subscriber sub);
    void publish(const Signal& signal);

private:
    std::vector<Subscriber> subscribers_;
    bool                    in_publish_{false};
};

} // namespace trade_bot
