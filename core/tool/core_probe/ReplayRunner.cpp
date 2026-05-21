#include "ReplayRunner.hpp"
#include "ProbePipeline.hpp"
#include "TraceLogger.hpp"
#include "AiRecipeRunner.hpp"

#include "perf/PerfRegistry.hpp"
#include "transport/IClock.hpp"
#include "transport/ReplayFeed.hpp"

#include <chrono>
#include <iostream>
#include <print>

namespace trade_bot::probe {

namespace {

// Returns system time but skips all sleep_until calls → as-fast-as-possible replay.
class NullSleepClock final : public IClock {
public:
    time_point now() const override { return std::chrono::system_clock::now(); }
    void sleep_until(time_point) override {}
};

} // namespace

int ReplayRunner::run(const CliOptions& opts) {
    if (opts.dump_path.empty()) {
        std::cerr << "Error: --dump is required for replay subcommand.\n";
        return 1;
    }
    if (opts.tickers.empty()) {
        std::cerr << "Error: --ticker is required for replay subcommand.\n";
        return 1;
    }

    // Initialize trace logger
    auto& logger = TraceLogger::instance();
    logger.init(opts);

    // Emit meta event
    logger.log_meta("1.0", GIT_SHA, opts.config_path);

    // Build pipeline
    ProbePipeline pipeline(opts);
    pipeline.summary().set_source("replay file=" + opts.dump_path);

    // Create replay feed
    auto clock = std::make_shared<NullSleepClock>();
    double speed = opts.speed_multiplier;
    ReplayFeed feed(opts.dump_path, clock, speed);

    if (opts.limit_messages > 0) {
        pipeline.set_limit_callback([&feed]() {
            feed.stop();
        });
    }

    // Register pipeline listeners (TickerControllers as IMarketDataListener)
    for (auto* listener : pipeline.get_listeners()) {
        feed.add_listener(listener);
    }
    // PaperExecutor also needs market data for position tracking
    if (!opts.no_executor) {
        feed.add_listener(&pipeline.paper_executor());
    }

    // Run replay
    if (!opts.machine) {
        std::cerr << "=== core_probe replay ===\n"
                  << "  dump:    " << opts.dump_path << "\n"
                  << "  tickers: ";
        for (size_t i = 0; i < opts.tickers.size(); ++i) {
            if (i > 0) std::cerr << ", ";
            std::cerr << opts.tickers[i];
        }
        std::cerr << "\n\n";
    }

    auto wall_start = std::chrono::steady_clock::now();
    auto stats = feed.run();
    auto wall_end = std::chrono::steady_clock::now();

    std::print(stderr, "[DEBUG ReplayRunner] messages_read={} messages_dispatched={} parse_errors={}\n", stats.messages_read, stats.messages_dispatched, stats.parse_errors);

    double wall_sec = std::chrono::duration<double>(wall_end - wall_start).count();
    pipeline.summary().set_wall_duration_sec(wall_sec);

    // Record feed stats
    for (size_t i = 0; i < stats.parse_errors; ++i) {
        pipeline.summary().record_parse_error();
    }

    // Finalize pipeline (drain executor, compute final stats)
    pipeline.finalize();

    // Check for strict invariant violation
    if (pipeline.invariants().is_strict() && pipeline.invariants().had_violation()) {
        logger.shutdown();
        return 4;
    }

    // Emit summary
    auto summary_json = pipeline.summary().to_json();
    {
        TraceEvent ev;
        ev.ts_ns = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
        ev.stage = "summary";
        ev.severity = "info";
        ev.payload = summary_json;
        ev.message = "Run complete";
        logger.enqueue(std::move(ev));
    }

    // Print human-readable summary (not in machine mode — it's in JSONL)
    if (!opts.machine && !opts.no_stdout) {
        // Small delay to let async logger flush trace events before summary
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        std::cout << "\n" << pipeline.summary().to_human_string() << "\n";

        // Print perf report
        std::string perf_report = PerfRegistry::instance().render_text_report();
        if (!perf_report.empty()) {
            std::cout << "\n=== Latency Report ===\n" << perf_report << "\n";
        }
    }

    logger.shutdown();

    // Optional: run an AI recipe over collected aggregates.  Output goes to
    // stdout as a single JSON object (deterministic), suitable for piping
    // into `jq` or capturing via `subprocess.run(...).stdout`.
    if (!opts.ai_recipe.empty()) {
        return AiRecipeRunner::run(opts.ai_recipe, opts, pipeline.summary());
    }

    return 0;
}

} // namespace trade_bot::probe
