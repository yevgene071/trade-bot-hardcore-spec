#pragma once

#include "CliOptions.hpp"

namespace trade_bot::probe {

class DiffRunner {
public:
    /// Compare two JSONL files. Returns 0 if identical, 2 if differences found, 1 on error.
    static int run(const CliOptions& opts);
};

} // namespace trade_bot::probe
