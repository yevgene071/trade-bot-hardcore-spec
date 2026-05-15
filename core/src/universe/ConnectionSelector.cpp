#include "ConnectionSelector.hpp"

namespace trade_bot {

ConnectionSelector::ConnectionSelector() : ConnectionSelector(Config{}) {}
ConnectionSelector::ConnectionSelector(Config cfg) : cfg_(cfg) {}

bool ConnectionSelector::eligible_(const ConnectionInfo& c) noexcept {
    return c.state == "Connected" && !c.view_mode;
}

std::optional<int> ConnectionSelector::select(
    const std::vector<ConnectionInfo>& conns) {
    auto it = std::find_if(conns.begin(), conns.end(), eligible_);
    if (it != conns.end()) {
        return it->id;
    }
    return std::nullopt;
}

}  // namespace trade_bot
