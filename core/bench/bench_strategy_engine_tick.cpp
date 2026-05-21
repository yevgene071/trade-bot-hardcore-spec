#include <benchmark/benchmark.h>
#include "strategy/StrategyEngine.hpp"
#include "strategy/IStrategy.hpp"
#include "strategy/TradePlan.hpp"
#include "signals/SignalBus.hpp"
#include "features/FeatureFrame.hpp"
#include <chrono>
#include <optional>
#include <string>

using namespace trade_bot;

namespace {

// Emits a stub plan on every tick to stress the full engine dispatch path.
class StubPlanStrategy final : public IStrategy {
public:
    StubPlanStrategy() : ticker_("BTCUSDT"), name_("StubPlan") {}

    const std::string& name()   const override { return name_; }
    const Ticker&      ticker() const override { return ticker_; }
    void on_frame(const FeatureFrame&) override {}
    std::optional<TradePlan> on_signal(const Signal&, std::chrono::system_clock::time_point) override { return std::nullopt; }

    std::optional<TradePlan> tick(std::chrono::system_clock::time_point) override {
        TradePlan p;
        p.ticker        = ticker_;
        p.strategy_name = FixedString<32>(name_.c_str());
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

static void BM_StrategyEngineTick(benchmark::State& state) {
    SignalBus      bus;
    StrategyEngine engine(bus);

    engine.add_strategy(std::make_unique<StubPlanStrategy>());

    int plans_seen = 0;
    engine.set_on_plan([&plans_seen](const TradePlan&) { ++plans_seen; });

    auto now = std::chrono::system_clock::now();

    for (auto _ : state) {
        engine.tick(now);
    }
    benchmark::DoNotOptimize(plans_seen);
}
BENCHMARK(BM_StrategyEngineTick);

BENCHMARK_MAIN();
