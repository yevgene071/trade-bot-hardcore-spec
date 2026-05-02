#pragma once

#include <chrono>
#include <string>

namespace trade_bot {

template<typename T>
struct FeedValue {
    T value;
    std::chrono::system_clock::time_point timestamp;
};

class IExternalFeed {
public:
    virtual ~IExternalFeed() = default;
    virtual std::string name() const = 0;
    virtual bool is_stale(std::chrono::seconds max_age) const = 0;
};

} // namespace trade_bot
