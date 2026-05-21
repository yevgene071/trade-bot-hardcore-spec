#pragma once

#include "CliOptions.hpp"

namespace trade_bot::probe {

/// `core_probe live` runner.  Wires `BeastWsClient` and `MarketDataFeed`
/// to the live WebSocket endpoint (without REST/discovery, as per V1 design).
struct LiveRunner {
    static int run(const CliOptions& opts);
};

} // namespace trade_bot::probe
