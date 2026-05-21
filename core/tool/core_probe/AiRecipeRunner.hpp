#pragma once

#include "CliOptions.hpp"
#include "SummaryCollector.hpp"

#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace trade_bot::probe {

/// Deterministic post-run analysis recipes for AI agents.
///
/// Each recipe consumes the in-memory `SummaryCollector` aggregates and
/// (optionally) does a second-pass over the JSONL trace on disk.  Output is
/// always a single JSON object printed to stdout.
class AiRecipeRunner {
public:
    /// Returns the list of supported recipe names.
    static std::vector<std::string> available_recipes();

    /// Run the named recipe.  Returns:
    ///   0 — success (JSON printed to stdout)
    ///   1 — unknown recipe (list printed to stderr)
    static int run(const std::string& recipe_name,
                   const CliOptions& opts,
                   const SummaryCollector& summary);

private:
    static nlohmann::json recipe_why_no_trade(const CliOptions& opts, const SummaryCollector& s);
    static nlohmann::json recipe_why_rejected(const CliOptions& opts, const SummaryCollector& s);
    static nlohmann::json recipe_slow_stage(const CliOptions& opts, const SummaryCollector& s);
    static nlohmann::json recipe_invariant_summary(const CliOptions& opts, const SummaryCollector& s);
    static nlohmann::json recipe_signal_strategy_gap(const CliOptions& opts, const SummaryCollector& s);
};

} // namespace trade_bot::probe
