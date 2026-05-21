#include "LiveRunner.hpp"
#include "AiRecipeRunner.hpp"
#include "ProbePipeline.hpp"
#include "TraceLogger.hpp"

#include "transport/BeastWsClient.hpp"
#include "transport/MarketDataFeed.hpp"
#include "perf/PerfRegistry.hpp"

#include <boost/asio/io_context.hpp>
#include <boost/asio/signal_set.hpp>
#include <boost/asio/steady_timer.hpp>
#include <chrono>
#include <iostream>
#include <memory>
#include <thread>

namespace trade_bot::probe {

namespace net = boost::asio;

#ifndef GIT_SHA
#define GIT_SHA "unknown"
#endif

namespace {

struct LiveContext {
    net::io_context ioc;
    std::shared_ptr<BeastWsClient> ws;
    std::unique_ptr<MarketDataFeed> feed;
    std::unique_ptr<ProbePipeline> pipeline;
    std::unique_ptr<net::steady_timer> timer;
    std::unique_ptr<net::signal_set> signals;
    CliOptions opts;
    std::chrono::system_clock::time_point start_time;
};

void schedule_tick(LiveContext& ctx) {
    ctx.timer->expires_after(std::chrono::milliseconds(50)); // 20 Hz
    ctx.timer->async_wait([&ctx](const boost::system::error_code& ec) {
        if (ec) return;

        auto now = std::chrono::system_clock::now();
        for (const auto& ticker : ctx.opts.tickers) {
            ctx.pipeline->drive_tick(ticker, now);
        }

        schedule_tick(ctx);
    });
}

} // namespace

int LiveRunner::run(const CliOptions& opts) {
    if (opts.tickers.empty()) {
        std::cerr << "Error: --ticker is required for live subcommand.\n";
        return 1;
    }

    auto& logger = TraceLogger::instance();
    logger.init(opts);
    logger.log_meta("1.0", GIT_SHA, opts.config_path);

    LiveContext ctx;
    ctx.opts = opts;
    ctx.start_time = std::chrono::system_clock::now();

    ctx.ws = std::make_shared<BeastWsClient>(ctx.ioc);
    ctx.feed = std::make_unique<MarketDataFeed>(ctx.ws, 1);

    ctx.pipeline = std::make_unique<ProbePipeline>(opts);
    ctx.pipeline->summary().set_source("live ws=" + opts.ws_url);

    if (opts.limit_messages > 0) {
        ctx.pipeline->set_limit_callback([&ctx]() {
            if (!ctx.opts.machine) {
                std::cerr << "\nLimit of " << ctx.opts.limit_messages << " messages reached. Stopping live run...\n";
            }
            ctx.ioc.stop();
        });
    }

    // Register listeners
    for (auto* listener : ctx.pipeline->get_listeners()) {
        ctx.feed->add_listener(listener);
    }
    if (!opts.no_executor) {
        ctx.feed->add_listener(&ctx.pipeline->paper_executor());
    }

    // Subscribe tickers
    for (const auto& ticker : opts.tickers) {
        ctx.feed->subscribe_ticker(ticker);
    }

    // Signals for graceful termination
    ctx.signals = std::make_unique<net::signal_set>(ctx.ioc, SIGINT, SIGTERM);
    ctx.signals->async_wait([&ctx](const boost::system::error_code& /*ec*/, int sig) {
        if (!ctx.opts.machine) {
            std::cerr << "\nReceived signal " << sig << ", shutting down live feed...\n";
        }
        ctx.ioc.stop();
    });

    // Tick timer
    ctx.timer = std::make_unique<net::steady_timer>(ctx.ioc);
    schedule_tick(ctx);

    if (!opts.machine) {
        std::cerr << "=== core_probe live ===\n"
                  << "  ws-url:  " << opts.ws_url << "\n"
                  << "  tickers: ";
        for (size_t i = 0; i < opts.tickers.size(); ++i) {
            if (i > 0) std::cerr << ", ";
            std::cerr << opts.tickers[i];
        }
        std::cerr << "\n\nPress Ctrl+C to stop...\n\n";
    }

    // Start feed and run io_context
    ctx.feed->start();
    ctx.ws->connect(opts.ws_url);

    auto wall_start = std::chrono::steady_clock::now();
    ctx.ioc.run();
    auto wall_end = std::chrono::steady_clock::now();

    // Stop feed
    ctx.feed->stop();
    ctx.ws->disconnect();

    double wall_sec = std::chrono::duration<double>(wall_end - wall_start).count();
    ctx.pipeline->summary().set_wall_duration_sec(wall_sec);

    ctx.pipeline->finalize();

    if (ctx.pipeline->invariants().is_strict() && ctx.pipeline->invariants().had_violation()) {
        logger.shutdown();
        return 4;
    }

    // Emit summary JSONL event
    auto summary_json = ctx.pipeline->summary().to_json();
    {
        TraceEvent ev;
        ev.ts_ns = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
        ev.stage = "summary";
        ev.severity = "info";
        ev.payload = summary_json;
        ev.message = "Live run complete";
        logger.enqueue(std::move(ev));
    }

    if (!opts.machine && !opts.no_stdout) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        std::cout << "\n" << ctx.pipeline->summary().to_human_string() << "\n";
        std::string perf_report = PerfRegistry::instance().render_text_report();
        if (!perf_report.empty()) {
            std::cout << "\n=== Latency Report ===\n" << perf_report << "\n";
        }
    }

    logger.shutdown();

    if (!opts.ai_recipe.empty()) {
        return AiRecipeRunner::run(opts.ai_recipe, opts, ctx.pipeline->summary());
    }
    return 0;
}

} // namespace trade_bot::probe
