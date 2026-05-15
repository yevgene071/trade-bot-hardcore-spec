#pragma once

#include "domain/Types.hpp"

#include <optional>
#include <vector>

namespace trade_bot {

/**
 * Selects the trading connection from `GET /api/connections`. A connection is
 * eligible only when `State == "Connected"` and `ViewMode == false`. In paper
 * mode the caller may set `prefer_demo` to bias selection toward demo
 * connections (DemoMode flag — modelled in ConnectionInfo as a field if/when
 * the API surface adds it).
 */
class ConnectionSelector {
public:
    struct Config {
        bool prefer_demo{false};   // reserved for the DemoMode flag (not yet in TickerInfo)
    };

    ConnectionSelector();
    explicit ConnectionSelector(Config cfg);

    /// Returns the chosen connection id or std::nullopt if no eligible one.
    static std::optional<int> select(const std::vector<ConnectionInfo>& conns);

private:
    static bool eligible_(const ConnectionInfo& c) noexcept;
    Config cfg_;
};

}  // namespace trade_bot
