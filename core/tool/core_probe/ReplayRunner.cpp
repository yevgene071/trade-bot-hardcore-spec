#include "ReplayRunner.hpp"
#include "ProbePipeline.hpp"
#include "TraceLogger.hpp"
#include "AiRecipeRunner.hpp"

#include "perf/PerfRegistry.hpp"
#include "transport/IClock.hpp"
#include "transport/ReplayFeed.hpp"
#include "MockRiskTestStrategy.hpp"
#include "signals/SignalBus.hpp"

#include <chrono>
#include <iostream>
#include <print>
#include <functional>
#include <memory>

namespace trade_bot::probe {

namespace {

// Returns system time but skips all sleep_until calls → as-fast-as-possible replay.
class NullSleepClock final : public IClock {
public:
    time_point now() const override { return std::chrono::system_clock::now(); }
    void sleep_until(time_point) override {}
};

class ReplayRiskVerifier final : public IMarketDataListener {
public:
    ReplayRiskVerifier(ProbePipeline& pipeline, ReplayFeed& feed)
        : pipeline_(pipeline), feed_(feed) {}

    void on_trade(const Ticker&, const Trade&) override {}
    void on_trades(const Ticker& ticker, const std::vector<Trade>& trades) override {
        for (const auto& t : trades) on_trade(ticker, t);
    }
    void on_orderbook_update(const OrderBookUpdate&) override {}
    void on_order_update(const OrderUpdate&) override {}
    void on_position_update(const PositionUpdate&) override {}
    void on_balance_update(const BalanceUpdate&) override {}
    void on_finres_update(const FinresUpdate&) override {}
    void on_error(const std::string&) override {}

    void on_orderbook_snapshot(const OrderBookSnapshot& snap) override {
        if (triggered_) return;
        triggered_ = true;

        auto ticker = snap.ticker;

        if (snap.bids.empty() || snap.asks.empty()) {
            std::cerr << "[ReplayRiskVerifier] Error: snapshot has empty bids/asks\n";
            feed_.stop();
            return;
        }

        double mid_price = (snap.bids[0].price + snap.asks[0].price) * 0.5;

        // Instantiate and register the mock strategy
        auto mock_strat = std::make_unique<MockRiskTestStrategy>(ticker);
        auto* mock_strat_ptr = mock_strat.get();
        pipeline_.strategy_engine().add_strategy(std::move(mock_strat));

        // Save initial account state
        const auto orig_account = pipeline_.account();

        // Trace Id buffer
        uint64_t next_trace_id = 999001;

        // Helper to run a test case
        auto run_test_case = [&](const std::string& name, const std::function<void(TradePlan&, AccountState&)>& setup_fn) {
            // Reset account to clean defaults
            pipeline_.account() = orig_account;

            // Construct standard valid baseline plan
            TradePlan plan;
            plan.ticker = ticker;
            plan.side = Side::Buy;
            plan.entry_type = OrderType::Limit;
            plan.entry_price = mid_price;
            plan.stop_price = mid_price * 0.999; // stop distance = 10 bps
            plan.tp1_price = mid_price * 1.001;  // TP distance = 10 bps (1.0 R:R)
            plan.tp1_size_ratio = 0.5;
            plan.size_coin = 1.0;
            plan.strategy_name = "MockRiskTestStrategy";
            plan.reason.assign("Risk Replay Test: " + name);
            plan.trace_id = next_trace_id++;
            plan.valid_until = std::chrono::system_clock::now() + std::chrono::seconds(10); // 10s valid

            // Apply setup adjustments
            setup_fn(plan, pipeline_.account());

            // Set the plan on mock strategy
            mock_strat_ptr->set_next_plan(plan);

            // Publish a dummy signal to trigger the strategy
            Signal sig;
            sig.ticker = ticker;
            sig.kind = SignalKind::LevelApproach;
            sig.price = mid_price;
            sig.trigger_trace_id = plan.trace_id;
            sig.timestamp = std::chrono::system_clock::now();
            sig.payload.touches = 1;
            sig.payload.speed_bps = 5.0;
            sig.payload.original_size = 10.0;
            sig.payload.size = 10.0;
            sig.payload.side = "Bid";

            // Dispatch signal through signal bus
            pipeline_.signal_bus().publish(sig);

            // Clear next plan
            mock_strat_ptr->set_next_plan(std::nullopt);
        };

        // Test Case 1: R1 Kill-Switch Active
        run_test_case("KillSwitchActive", [](TradePlan&, AccountState& state) {
            state.kill_switch_triggered = true;
        });

        // Test Case 2: R2 Daily Loss Limit Hit
        run_test_case("DailyLossLimitHit", [](TradePlan&, AccountState& state) {
            state.realized_pnl_today_usd = -state.starting_equity_usd * 0.5;
        });

        // Test Case 3: R3 Too Many Positions
        run_test_case("TooManyPositions", [](TradePlan&, AccountState& state) {
            state.active_positions = 100;
        });

        // Test Case 4: R6 Stop Too Tight
        run_test_case("StopTooTight", [mid_price](TradePlan& plan, AccountState&) {
            plan.stop_price = mid_price * 0.9998;
        });

        // Test Case 5: R6 Stop Too Wide
        run_test_case("StopTooWide", [mid_price](TradePlan& plan, AccountState&) {
            plan.stop_price = mid_price * 0.9900;
        });

        // Test Case 6: R7 TP1 R:R Too Low
        run_test_case("PoorRewardRisk", [mid_price](TradePlan& plan, AccountState&) {
            plan.tp1_price = mid_price * 1.0001;
        });

        // Test Case 7: R8 Size Below Minimum
        run_test_case("SizeBelowMinimum", [](TradePlan&, AccountState& state) {
            state.equity_usd = 0.01;
        });

        // Test Case 8: R9 Insufficient Margin
        run_test_case("InsufficientMargin", [](TradePlan&, AccountState& state) {
            state.free_balance_usd = 1.0;
        });

        // Clean up
        pipeline_.account() = orig_account;
        pipeline_.strategy_engine().remove_strategy(ticker, "MockRiskTestStrategy");

        // Stop replay
        feed_.stop();
    }

private:
    ProbePipeline& pipeline_;
    ReplayFeed& feed_;
    bool triggered_{false};
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
    // TODO: wire --start-ts to ReplayFeed::skip_until() once that API exists

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

    std::unique_ptr<ReplayRiskVerifier> risk_verifier;
    if (opts.scenario == "risk_limit_rejection") {
        risk_verifier = std::make_unique<ReplayRiskVerifier>(pipeline, feed);
        feed.add_listener(risk_verifier.get());
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

    // Record parse errors from ReplayFeed stats (ReplayFeed does NOT call
    // on_error for data-parse failures — only increments stats_.parse_errors).
    // BookTraceListener::on_error covers transport-level errors (MarketDataFeed).
    for (size_t i = 0; i < stats.parse_errors; ++i) {
        pipeline.summary().record_parse_error();
    }

    // Finalize pipeline (drain executor, compute final stats, emit summary event)
    pipeline.finalize();

    // Check for strict invariant violation
    if (pipeline.invariants().is_strict() && pipeline.invariants().had_violation()) {
        logger.shutdown();
        return 4;
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
