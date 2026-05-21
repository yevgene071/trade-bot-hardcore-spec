#include <benchmark/benchmark.h>
#include "features/FeatureExtractor.hpp"
#include "marketdata/OrderBook.hpp"
#include "marketdata/TradeStream.hpp"
#include "signals/DensityDetector.hpp"
#include "signals/SignalBus.hpp"
#include "strategy/IStrategy.hpp"
#include "strategy/StrategyEngine.hpp"
#include "strategy/TradePlan.hpp"
#include "executor/IExecutor.hpp"
#include "perf/LatencyTracer.hpp"
#include "perf/PerfRegistry.hpp"
#include "perf/TraceContext.hpp"
#include "perf/TraceTimeBuffer.hpp"
#include "universe/TickerUniverse.hpp"
#include <chrono>
#include <map>
#include <optional>
#include <random>
#include <string>

using namespace trade_bot;

namespace {

// Minimal no-op executor for benchmarking (avoids PaperExecutor initialization overhead/segfaults)
class NoOpExecutor final : public IExecutor {
public:
    void submit(const TradePlan&) override {}
    void cancel_all(const Ticker&) override {}
    void inject_recovered_trades(const std::vector<ActiveTrade>&) override {}
    void close_trade(const Ticker&, const FixedString<32>&) override {}
    void tick(std::chrono::system_clock::time_point) override {}
    std::vector<ActiveTrade> get_active_trades() const override { return {}; }
    std::vector<ClosedTrade> pop_closed_trades() override { return {}; }
    void set_mark_prices(const absl::btree_map<Ticker, double>&) override {}
    
    // IMarketDataListener
    void on_trade(const Ticker&, const Trade&) override {}
    void on_orderbook_snapshot(const OrderBookSnapshot&) override {}
    void on_orderbook_update(const OrderBookUpdate&) override {}
    void on_order_update(const OrderUpdate&) override {}
    void on_position_update(const PositionUpdate&) override {}
    void on_balance_update(const BalanceUpdate&) override {}
    void on_finres_update(const FinresUpdate&) override {}
    void on_error(const std::string&) override {}
};

// Always emits a plan to maximise E2E path coverage.
class AlwaysPlanStrategy final : public IStrategy {
public:
    AlwaysPlanStrategy() : ticker_("BTCUSDT"), name_("AlwaysPlan") {}

    const std::string& name()   const override { return name_; }
    const Ticker&      ticker() const override { return ticker_; }
    void on_frame(const FeatureFrame&) override {}
    std::optional<TradePlan> on_signal(const Signal&, std::chrono::system_clock::time_point) override { return std::nullopt; }

    std::optional<TradePlan> tick(std::chrono::system_clock::time_point) override {
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

    bool          has_active_plan() const override { return false; }
    void          reset_active_plan()   override {}
    StrategyState get_state()       const override { return {}; }

private:
    Ticker      ticker_;
    std::string name_;
};

} // namespace

static void BM_EndToEndPipeline(benchmark::State& state) {
    OrderBook        ob{"BTCUSDT", 0.01, 1e-8};
    TradeStream      ts{"BTCUSDT"};
    FeatureExtractor extractor{"BTCUSDT"};
    extractor.set_sources(&ob, &ts);

    TickerUniverse  universe;
    SignalBus       bus;
    DensityDetector density{"BTCUSDT", bus, ob, universe};

    StrategyEngine engine(bus);
    engine.add_strategy(std::make_unique<AlwaysPlanStrategy>());

    NoOpExecutor executor;

    auto& e2e_hist = PerfRegistry::instance().get_or_create(kStageEndToEnd);

    engine.set_on_plan([&](const TradePlan& plan) {
        auto opt = trace_times().lookup(plan.trace_id);
        if (opt) {
            auto now_ns = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count());
            auto delta_us = static_cast<int64_t>((now_ns - *opt) / 1000u);
            e2e_hist.record(delta_us);
        }
        executor.submit(plan);
    });

    // Pre-build synthetic events (power-of-2 size for cheap modulo via &).
    std::mt19937_64 rng{42};
    std::uniform_real_distribution<double> price_dist(99900.0, 100100.0);
    std::uniform_real_distribution<double> size_dist(0.1, 10.0);
    std::uniform_int_distribution<int>     side_dist(0, 1);

    constexpr size_t kN = 4096;
    std::vector<OrderBookUpdate> events;
    events.reserve(kN);
    auto ts_now = std::chrono::system_clock::now();
    for (size_t i = 0; i < kN; ++i) {
        double p = std::round(price_dist(rng) * 100.0) / 100.0;
        OrderBookUpdate upd;
        upd.ticker  = "BTCUSDT";
        upd.changes = {{p, size_dist(rng),
                        side_dist(rng) == 0 ? Side::Buy : Side::Sell}};
        upd.ts = ts_now;
        events.push_back(upd);
        ob.apply_update(upd); // warmup: populate book
    }

    size_t idx = 0;
    for (auto _ : state) {
        const auto& upd = events[idx++ & (kN - 1)];

        auto trace_id = next_trace_id();
        auto recv_ns  = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
        trace_times().store(trace_id, recv_ns);
        TraceContextScope scope(trace_id, recv_ns);

        ob.apply_update(upd);
        density.on_book_update(upd);
        auto frame = extractor.extract(upd.ts);
        density.on_frame(frame);
        engine.on_frame(frame);
        engine.tick(upd.ts);
    }

    state.counters["e2e_p99_us"] =
        e2e_hist.value_at_percentile(99.0);
}
BENCHMARK(BM_EndToEndPipeline);

BENCHMARK_MAIN();
