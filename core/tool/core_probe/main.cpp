// core_probe — AI-friendly diagnostic CLI for core/ pipeline tracing & error tracking.
//
// Usage:
//   core_probe replay --dump <ndjson> --ticker T[,T...] [common flags]
//   core_probe live   --ws-url <url>  --ticker T[,T...] [common flags]
//   core_probe synth  --scenario <name>                 [common flags]
//   core_probe diff   --left <jsonl1> --right <jsonl2>
//   core_probe assert --dump <ndjson> --ticker T --expect EXPR[,EXPR...]
//   core_probe schema

#include "logger/Logger.hpp"
#include "CliOptions.hpp"
#include "TraceLogger.hpp"
#include "ProbePipeline.hpp"
#include "SummaryCollector.hpp"
#include "InvariantChecker.hpp"
#include "ReplayRunner.hpp"
#include "DiffRunner.hpp"
#include "AssertRunner.hpp"
#include "SchemaRunner.hpp"
#include "SynthRunner.hpp"
#include "LiveRunner.hpp"

#include "config/Config.hpp"

#include <iostream>
#include <filesystem>
#include <chrono>

namespace {

using namespace trade_bot::probe;

constexpr const char* kVersion = "0.0.1";

// Git SHA is injected at build time via -DGIT_SHA=...
#ifndef GIT_SHA
#define GIT_SHA "unknown"
#endif

void print_stages() {
    std::cout << "Available trace stages:\n"
              << "  meta       — schema, git_sha, config info (first event in machine mode)\n"
              << "  ws_raw     — raw WS message / NDJSON line\n"
              << "  book       — orderbook apply + mid/spread summary\n"
              << "  trades     — each trade print (or throttled summary)\n"
              << "  features   — FeatureFrame fields (mid, spread_bps, vol, imbalance, etc.)\n"
              << "  signal     — signal published to SignalBus (kind + payload)\n"
              << "  strategy   — strategy on_signal/tick, TradePlan emission\n"
              << "  risk       — RiskDecision: accepted/rejected + reason\n"
              << "  executor   — submit, fill, cancel, close_trade\n"
              << "  account    — equity/balance/position changes\n"
              << "  invariant  — invariant violation (book crossed, NaN, etc.)\n"
              << "  error      — unhandled exception or unexpected error\n"
              << "  perf       — per-stage latency histograms\n"
              << "  summary    — final aggregate: counts, PnL, latency\n";
}

int run_replay(const CliOptions& opts) {
    return ReplayRunner::run(opts);
}

int run_diff(const CliOptions& opts) {
    return DiffRunner::run(opts);
}

int run_assert(const CliOptions& opts) {
    return AssertRunner::run(opts);
}

int run_schema(const CliOptions& /*opts*/) {
    return SchemaRunner::run();
}

int run_synth(const CliOptions& opts) {
    return SynthRunner::run(opts);
}

int run_live(const CliOptions& opts) {
    return LiveRunner::run(opts);
}

} // namespace

int main(int argc, char* argv[]) {
    auto opts = CliOptions::parse(argc, argv);

    // Handle early-exit flags
    if (opts.show_version) {
        std::cout << "core_probe v" << kVersion << " (git: " << GIT_SHA << ")\n";
        return 0;
    }

    if (opts.list_stages) {
        print_stages();
        return 0;
    }

    if (opts.cmd == Subcommand::None) {
        std::cerr << "Error: no subcommand specified.\n\n";
        opts.print_help();
        return 1;
    }

    // Validate output combination
    if (opts.machine && opts.no_jsonl && opts.no_stdout) {
        std::cerr << "Error: --machine + --no-jsonl + --no-stdout = nothing to write.\n";
        return 1;
    }

    // Load config if it exists (non-fatal if missing)
    if (std::filesystem::exists(opts.config_path)) {
        try {
            trade_bot::Config::load(opts.config_path);
        } catch (const std::exception& e) {
            std::cerr << "Warning: failed to load config '" << opts.config_path
                      << "': " << e.what() << "\n";
        }
    } else if (!opts.machine) {
        std::cerr << "Warning: config file '" << opts.config_path
                  << "' not found, using defaults.\n";
    }

    // Init spdlog to a file (not stdout — that belongs to trace output)
    trade_bot::Logger::init("logs/core-probe.log", "info");

    // Route to subcommand
    switch (opts.cmd) {
        case Subcommand::Replay:
            return run_replay(opts);
        case Subcommand::Live:
            return run_live(opts);
        case Subcommand::Synth:
            return run_synth(opts);
        case Subcommand::Diff:
            return run_diff(opts);
        case Subcommand::Assert:
            return run_assert(opts);
        case Subcommand::Schema:
            return run_schema(opts);
        default:
            std::cerr << "Error: unknown subcommand.\n";
            return 1;
    }
}
