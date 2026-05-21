#pragma once

#include "CliOptions.hpp"

namespace trade_bot::probe {

/// `core_probe synth` runner.  Drives a `SyntheticFeed` through a full
/// `ProbePipeline` (book → features → signals → strategy → risk → executor)
/// and emits the same trace/summary stream as `replay`.
struct SynthRunner {
    static int run(const CliOptions& opts);
};

} // namespace trade_bot::probe
