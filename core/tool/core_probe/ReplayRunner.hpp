#pragma once

#include "CliOptions.hpp"

namespace trade_bot::probe {

class ReplayRunner {
public:
    /// Run the replay subcommand. Returns exit code.
    static int run(const CliOptions& opts);
};

} // namespace trade_bot::probe
