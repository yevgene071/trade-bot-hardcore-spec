#include "SynthRunner.hpp"
#include "AiRecipeRunner.hpp"
#include "ProbePipeline.hpp"
#include "SyntheticFeed.hpp"
#include "TraceLogger.hpp"

#include "perf/PerfRegistry.hpp"

#include <chrono>
#include <iostream>

namespace trade_bot::probe {

#ifndef GIT_SHA
#define GIT_SHA "unknown"
#endif

int SynthRunner::run(const CliOptions& opts) {
    if (opts.scenario.empty()) {
        std::cerr << "Error: --scenario is required for synth subcommand.\n"
                  << "Available scenarios:\n";
        for (const auto& s : SyntheticFeed::available_scenarios()) {
            std::cerr << "  " << s << "\n";
        }
        return 1;
    }
    if (!SyntheticFeed::is_known_scenario(opts.scenario)) {
        std::cerr << "Error: unknown scenario '" << opts.scenario << "'.\n"
                  << "Available scenarios:\n";
        for (const auto& s : SyntheticFeed::available_scenarios()) {
            std::cerr << "  " << s << "\n";
        }
        return 1;
    }

    // Synth always runs against a single fixed synthetic ticker.  If the user
    // supplied --ticker, use the first entry; otherwise default to "SYNTH".
    CliOptions eff = opts;
    if (eff.tickers.empty()) {
        eff.tickers = {"SYNTH"};
    } else {
        // Synth scenarios operate on a single ticker
        eff.tickers = { eff.tickers.front() };
    }

    // ── Build pipeline ───────────────────────────────────────────────────────
    auto& logger = TraceLogger::instance();
    logger.init(eff);
    logger.log_meta("1.0", GIT_SHA, eff.config_path);

    ProbePipeline pipeline(eff);
    pipeline.summary().set_source("synth scenario=" + eff.scenario);

    SyntheticFeed feed(eff.scenario, eff.tickers.front());
    for (auto* l : pipeline.get_listeners()) {
        feed.add_listener(l);
    }
    if (!eff.no_executor) {
        feed.add_listener(&pipeline.paper_executor());
    }

    if (eff.limit_messages > 0) {
        pipeline.set_limit_callback([&feed]() {
            feed.stop();
        });
    }

    // Drive a pipeline tick after every dispatched event so detectors /
    // strategies see consistent state.
    feed.set_tick_callback([&pipeline](const Ticker& tkr,
                                       std::chrono::system_clock::time_point ts) {
        pipeline.drive_tick(tkr, ts);
    });

    if (!eff.machine) {
        std::cerr << "=== core_probe synth ===\n"
                  << "  scenario: " << eff.scenario << "\n"
                  << "  ticker:   " << eff.tickers.front() << "\n\n";
    }

    auto wall_start = std::chrono::steady_clock::now();
    [[maybe_unused]] auto stats = feed.run(&pipeline);
    auto wall_end = std::chrono::steady_clock::now();

    double wall_sec = std::chrono::duration<double>(wall_end - wall_start).count();
    pipeline.summary().set_wall_duration_sec(wall_sec);
    // Messages are already counted by BookTraceListener::check_limit() —
    // do NOT double-count via a manual loop over stats.messages_dispatched.

    // Finalize pipeline (drain executor, compute final stats, emit summary event)
    pipeline.finalize();

    if (pipeline.invariants().is_strict() && pipeline.invariants().had_violation()) {
        logger.shutdown();
        return 4;
    }

    if (!eff.machine && !eff.no_stdout) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        std::cout << "\n" << pipeline.summary().to_human_string() << "\n";
        std::string perf_report = PerfRegistry::instance().render_text_report();
        if (!perf_report.empty()) {
            std::cout << "\n=== Latency Report ===\n" << perf_report << "\n";
        }
    }

    logger.shutdown();

    if (!eff.ai_recipe.empty()) {
        return AiRecipeRunner::run(eff.ai_recipe, eff, pipeline.summary());
    }
    return 0;
}

} // namespace trade_bot::probe
