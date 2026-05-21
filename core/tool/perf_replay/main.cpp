// T0-REPLAY: perf_replay — offline pipeline latency profiler.
// Replays an NDJSON dump through the full instrument pipeline and emits
// per-stage HdrHistogram percentile reports via PerfRegistry.
//
// Usage:
//   perf_replay --dump replay/dumps/btcusdt_5min_sample.ndjson
//               --ticker BTCUSDT [--ticker ETHUSDT ...]
//               [--report logs/perf-replay-<ts>.txt]

#include "logger/Logger.hpp"
#include "control/TickerController.hpp"
#include "executor/PaperExecutor.hpp"
#include "marketdata/ClusterSnapshot.hpp"
#include "perf/LatencyTracer.hpp"
#include "perf/PerfRegistry.hpp"
#include "perf/TraceContext.hpp"
#include "perf/TraceTimeBuffer.hpp"
#include "risk/AccountState.hpp"
#include "risk/NewsCalendar.hpp"
#include "risk/RiskManager.hpp"
#include "signals/SignalBus.hpp"
#include "strategy/IStrategy.hpp"
#include "strategy/StrategyEngine.hpp"
#include "strategy/TradePlan.hpp"
#include "transport/ClusterSnapshotClient.hpp"
#include "transport/IClock.hpp"
#include "transport/IHttpClient.hpp"
#include "transport/ReplayFeed.hpp"
#include "universe/TickerUniverse.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace {

using namespace trade_bot;
using namespace std::chrono;

// ── Offline stubs ─────────────────────────────────────────────────────────────

class NullHttpClient final : public IHttpClient {
public:
    HttpResponse get(const std::string&) override              { return {0, "", {}}; }
    HttpResponse post(const std::string&, const std::string&) override { return {0, "", {}}; }
    HttpResponse put(const std::string&, const std::string&) override  { return {0, "", {}}; }
    HttpResponse del(const std::string&) override              { return {0, "", {}}; }
    void set_timeout_ms(int) override {}
};

// Returns system time but skips all sleep_until calls → as-fast-as-possible replay.
class NullSleepClock final : public IClock {
public:
    time_point now() const override { return system_clock::now(); }
    void sleep_until(time_point) override {}
};

// ── TracingListenerAdapter ────────────────────────────────────────────────────
// Interposes between ReplayFeed and a TickerController.
// Generates a TraceId before each event, pushes TraceContextScope,
// delegates to the inner controller, then drives feature extraction
// and strategy dispatch.

class TracingListenerAdapter final : public IMarketDataListener {
public:
    TracingListenerAdapter(TickerController& ctrl, StrategyEngine& engine)
        : ctrl_(ctrl), engine_(engine) {}

    void on_orderbook_snapshot(const OrderBookSnapshot& s) override {
        auto [id, ns] = make_trace();
        trace_times().store(id, ns);
        TraceContextScope scope(id, ns);
        ctrl_.on_orderbook_snapshot(s);
        drive(s.ts);
    }

    void on_orderbook_update(const OrderBookUpdate& u) override {
        auto [id, ns] = make_trace();
        trace_times().store(id, ns);
        TraceContextScope scope(id, ns);
        ctrl_.on_orderbook_update(u);
        drive(u.ts);
    }

    void on_trade(const Ticker& t, const Trade& tr) override {
        auto [id, ns] = make_trace();
        trace_times().store(id, ns);
        TraceContextScope scope(id, ns);
        ctrl_.on_trade(t, tr);
        // trades alone don't tick strategy — book update drives tick cadence
    }

    void on_order_update(const OrderUpdate&)      override {}
    void on_position_update(const PositionUpdate&) override {}
    void on_balance_update(const BalanceUpdate&)   override {}
    void on_finres_update(const FinresUpdate&)     override {}
    void on_error(const std::string&)              override {}

private:
    static std::pair<TraceId, uint64_t> make_trace() noexcept {
        auto id = next_trace_id();
        auto ns = static_cast<uint64_t>(
            duration_cast<nanoseconds>(
                steady_clock::now().time_since_epoch()).count());
        return {id, ns};
    }

    void drive(system_clock::time_point ts) {
        auto frame = ctrl_.tick(ts);
        engine_.on_frame(frame);
        engine_.tick(ts);
    }

    TickerController& ctrl_;
    StrategyEngine&   engine_;
};

// ── StubPerfStrategy ──────────────────────────────────────────────────────────
// Emits a minimal TradePlan every kInterval ticks so that the
// StrategyEngine → RiskManager → PaperExecutor path is exercised and
// kStageEndToEnd gets populated.

class StubPerfStrategy final : public IStrategy {
public:
    explicit StubPerfStrategy(Ticker t)
        : ticker_(std::move(t)), name_("StubPerf") {}

    const std::string& name()   const override { return name_; }
    const Ticker&      ticker() const override { return ticker_; }
    void on_frame(const FeatureFrame&) override {}
    std::optional<TradePlan> on_signal(const Signal&, std::chrono::system_clock::time_point) override { return std::nullopt; }

    std::optional<TradePlan> tick(system_clock::time_point) override {
        if (++n_ % kInterval != 0) return std::nullopt;
        TradePlan p;
        p.ticker        = ticker_;
        p.strategy_name = FixedString<32>(name_.c_str());
        p.trace_id      = current_trace_context().trace_id;
        p.side          = Side::Buy;
        p.entry_price   = 100.0;
        p.stop_price    =  99.0;
        p.tp1_price     = 102.0;
        return p;
    }

    bool        has_active_plan() const override { return false; }
    void        reset_active_plan()   override {}
    StrategyState get_state()   const override { return {}; }

private:
    static constexpr int kInterval = 200;
    Ticker      ticker_;
    std::string name_;
    int         n_{0};
};

// ── Report writer ─────────────────────────────────────────────────────────────

void dump_report(const std::string& path) {
    std::string report = "=== Perf Replay Report ===\n"
                       + PerfRegistry::instance().render_text_report();
    std::cout << report << "\n";

    auto parent = std::filesystem::path(path).parent_path();
    if (!parent.empty()) std::filesystem::create_directories(parent);

    std::ofstream out(path);
    out << report;
}

} // namespace

int main(int argc, char* argv[]) {
    std::string              dump_path;
    std::vector<std::string> tickers;
    std::string              report_path;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if      (a == "--dump"   && i + 1 < argc) dump_path           = argv[++i];
        else if (a == "--ticker" && i + 1 < argc) tickers.push_back(argv[++i]);
        else if (a == "--report" && i + 1 < argc) report_path         = argv[++i];
    }

    if (dump_path.empty() || tickers.empty()) {
        std::cerr << "Usage: perf_replay --dump <ndjson> --ticker <TICKER> "
                     "[--ticker ...] [--report <path>]\n";
        return 1;
    }

    Logger::init("logs/perf-replay.log", "info");

    if (report_path.empty()) {
        auto ms = duration_cast<milliseconds>(
            system_clock::now().time_since_epoch()).count();
        report_path = "logs/perf-replay-" + std::to_string(ms) + ".txt";
    }

    std::cout << "=== perf_replay ===\n"
              << "  dump:   " << dump_path   << "\n"
              << "  report: " << report_path << "\n";
    for (const auto& t : tickers) std::cout << "  ticker: " << t << "\n";
    std::cout << "\n";

    // ── Offline infrastructure ────────────────────────────────────────────────
    NullHttpClient         null_http;
    ClusterSnapshotClient  null_cluster(null_http, "", 0);
    ClusterSnapshotManager cluster_mgr(null_cluster); // not started → no polling

    TickerUniverse universe;
    NewsCalendar   news;
    RiskManager    risk_mgr(universe, news);

    SignalBus      bus;
    StrategyEngine engine(bus);

    std::map<Ticker, const OrderBook*>                    books;
    std::vector<std::unique_ptr<TickerController>>        controllers;
    std::vector<std::unique_ptr<TracingListenerAdapter>>  adapters;

    for (const auto& t : tickers) {
        auto ctrl = std::make_unique<TickerController>(t, bus, universe, cluster_mgr);
        books[t] = ctrl->book.get();
        adapters.push_back(std::make_unique<TracingListenerAdapter>(*ctrl, engine));
        controllers.push_back(std::move(ctrl));
        engine.add_strategy(std::make_unique<StubPerfStrategy>(t));
    }

    PaperExecutor paper_exec(books);

    AccountState account;
    account.equity_usd         = 100'000.0;
    account.starting_equity_usd = 100'000.0;
    account.free_balance_usd   = 100'000.0;

    auto& e2e_hist = PerfRegistry::instance().get_or_create(kStageEndToEnd);

    engine.set_on_plan([&](const TradePlan& plan) {
        // End-to-end: from WS receive to plan dispatch.
        auto opt_recv_ns = trace_times().lookup(plan.trace_id);
        if (opt_recv_ns) {
            auto now_ns = static_cast<uint64_t>(
                duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count());
            auto delta_us = static_cast<int64_t>((now_ns - *opt_recv_ns) / 1000u);
            if (delta_us > 0) e2e_hist.record(delta_us);
        }

        RiskDecision decision;
        {
            LatencyTracer t_risk(PerfRegistry::instance().get_or_create(kStagePlanToRisk));
            decision = risk_mgr.evaluate(plan, account);
        }
        if (decision.accepted) {
            LatencyTracer t_sub(PerfRegistry::instance().get_or_create(kStageRiskToSubmit));
            paper_exec.submit(plan);
        }
    });

    // ── Replay ───────────────────────────────────────────────────────────────
    auto clock = std::make_shared<NullSleepClock>();
    ReplayFeed feed(dump_path, clock, /*speed_multiplier=*/0.0);

    for (auto& a : adapters)  feed.add_listener(a.get());
    feed.add_listener(&paper_exec);

    auto stats = feed.run();

    std::cout << "Replay complete: " << stats.messages_dispatched
              << " dispatched, " << stats.parse_errors << " errors\n\n";

    dump_report(report_path);
    return 0;
}
