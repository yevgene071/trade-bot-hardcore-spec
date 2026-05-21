#pragma once

#include "CliOptions.hpp"

namespace trade_bot::probe {

class AssertRunner {
public:
    /// Run replay and verify expectations. Returns 0 if all pass, 3 if failures, 1 on error.
    static int run(const CliOptions& opts);
};

} // namespace trade_bot::probe
