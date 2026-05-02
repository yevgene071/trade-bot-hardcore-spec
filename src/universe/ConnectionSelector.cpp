#include "ConnectionSelector.hpp"

namespace trade_bot {

ConnectionSelector::ConnectionSelector() : ConnectionSelector(Config{}) {}
ConnectionSelector::ConnectionSelector(Config cfg) : cfg_(cfg) {}

bool ConnectionSelector::eligible_(const ConnectionInfo& c) noexcept {
    return c.state == "Connected" && !c.view_mode;
}

std::optional<int> ConnectionSelector::select(
    const std::vector<ConnectionInfo>& conns) const {
    // First pass: eligible connections only.
    for (const auto& c : conns) {
        if (eligible_(c)) {
            return c.id;
        }
    }
    return std::nullopt;
}

}  // namespace trade_bot
