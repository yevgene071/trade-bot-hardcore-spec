#include "SyntheticFeed.hpp"
#include "perf/TraceContext.hpp"
#include "MockRiskTestStrategy.hpp"
#include "signals/SignalBus.hpp"
#include "ProbePipeline.hpp"

#include <algorithm>
#include <cmath>

namespace trade_bot::probe {

using namespace std::chrono;

namespace {

/// Synthetic origin epoch — fixed so two runs of the same scenario produce
/// identical timestamps.  Determinism is non-negotiable (see AGENTS.md §5).
constexpr int64_t kSyntheticEpochNs = 1'700'000'000'000'000'000LL;

system_clock::time_point ns_offset(int64_t off_ns) {
    return system_clock::time_point(nanoseconds(kSyntheticEpochNs + off_ns));
}

/// Build a price level with given side.
PriceLevel lvl(double price, double size, Side side) { return {price, size, side}; }

} // namespace

// ── Public API ───────────────────────────────────────────────────────────────

SyntheticFeed::SyntheticFeed(const std::string& scenario_name,
                             const Ticker& ticker)
    : scenario_(scenario_name), ticker_(ticker) {}

std::vector<std::string> SyntheticFeed::available_scenarios() {
    return {
        "density_appears",
        "density_eaten_then_breakout",
        "leader_moves_alt_lags",
        "risk_limit_rejection",
    };
}

bool SyntheticFeed::is_known_scenario(const std::string& name) {
    auto all = available_scenarios();
    return std::find(all.begin(), all.end(), name) != all.end();
}

SyntheticFeed::RunStats SyntheticFeed::run(ProbePipeline* pipeline) {
    RunStats stats;
    if (scenario_ == "density_appears") {
        run_density_appears(stats);
    } else if (scenario_ == "density_eaten_then_breakout") {
        run_density_eaten_then_breakout(stats);
    } else if (scenario_ == "leader_moves_alt_lags") {
        run_leader_moves_alt_lags(stats);
    } else if (scenario_ == "risk_limit_rejection") {
        run_risk_limit_rejection(stats, pipeline);
    }
    return stats;
}

// ── Dispatch helpers ─────────────────────────────────────────────────────────

void SyntheticFeed::dispatch_snapshot(const OrderBookSnapshot& s) {
    uint64_t ts_ns = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(s.ts.time_since_epoch()).count());
    TraceContextScope scope(ts_ns, ts_ns);
    for (auto* l : listeners_) l->on_orderbook_snapshot(s);
}

void SyntheticFeed::dispatch_update(const OrderBookUpdate& u) {
    uint64_t ts_ns = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(u.ts.time_since_epoch()).count());
    TraceContextScope scope(ts_ns, ts_ns);
    for (auto* l : listeners_) l->on_orderbook_update(u);
}

void SyntheticFeed::dispatch_trade(const Trade& t) {
    uint64_t ts_ns = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(t.timestamp.time_since_epoch()).count());
    TraceContextScope scope(ts_ns, ts_ns);
    for (auto* l : listeners_) l->on_trade(ticker_, t);
}

void SyntheticFeed::notify_tick(system_clock::time_point ts) {
    if (tick_cb_) {
        uint64_t ts_ns = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(ts.time_since_epoch()).count());
        TraceContextScope scope(ts_ns, ts_ns);
        tick_cb_(ticker_, ts);
    }
}

// ── Scenario: density_appears ────────────────────────────────────────────────
//
// Build a normal book, then drop a large size on the best bid → DensityDetected.
//
void SyntheticFeed::run_density_appears(RunStats& stats) {
    // 1. Initial snapshot — symmetric book around 100.00 with thin levels.
    OrderBookSnapshot snap;
    snap.ticker = ticker_;
    snap.ts = ns_offset(0);
    for (int i = 0; i < 10; ++i) {
        double bid_p = 100.00 - i * 0.01;
        double ask_p = 100.01 + i * 0.01;
        snap.bids.push_back(lvl(bid_p, 5.0, Side::Buy));
        snap.asks.push_back(lvl(ask_p, 5.0, Side::Sell));
    }
    dispatch_snapshot(snap);
    ++stats.snapshots; ++stats.messages_dispatched;
    notify_tick(snap.ts);

    // 2. A few harmless updates to warm up the detectors.
    for (int step = 1; step <= 20; ++step) {
        OrderBookUpdate u;
        u.ticker = ticker_;
        u.ts = ns_offset(step * 100'000'000LL); // +100ms each
        // Small refresh on best bid: keep size at 5.0 (no-op semantic; triggers apply path).
        u.changes.push_back(lvl(100.00, 5.0, Side::Buy));
        dispatch_update(u);
        ++stats.updates; ++stats.messages_dispatched;
        notify_tick(u.ts);
    }

    // 3. Density appears: huge bid (300 lots) at best bid.
    OrderBookUpdate big;
    big.ticker = ticker_;
    big.ts = ns_offset(21 * 100'000'000LL);
    big.changes.push_back(lvl(100.00, 300.0, Side::Buy));
    dispatch_update(big);
    ++stats.updates; ++stats.messages_dispatched;
    notify_tick(big.ts);

    // 4. Quiet ticks so the detector / strategy pipeline gets driven.
    for (int step = 22; step <= 40; ++step) {
        notify_tick(ns_offset(step * 100'000'000LL));
    }
}

// ── Scenario: density_eaten_then_breakout ────────────────────────────────────
//
// Density appears → market trades eat through it → breakout up.
// Expected signal sequence (with default detector config):
//   DensityDetected → DensityEating (or DensityRemoved) → LevelBreak.
//
void SyntheticFeed::run_density_eaten_then_breakout(RunStats& stats) {
    // 1. Snapshot
    OrderBookSnapshot snap;
    snap.ticker = ticker_;
    snap.ts = ns_offset(0);
    for (int i = 0; i < 10; ++i) {
        snap.bids.push_back(lvl(100.00 - i * 0.01, 5.0, Side::Buy));
        snap.asks.push_back(lvl(100.01 + i * 0.01, 5.0, Side::Sell));
    }
    dispatch_snapshot(snap);
    ++stats.snapshots; ++stats.messages_dispatched;
    notify_tick(snap.ts);

    // 2. Density placed on ask (250 lots @ 100.01).
    OrderBookUpdate place;
    place.ticker = ticker_;
    place.ts = ns_offset(500'000'000LL);
    place.changes.push_back(lvl(100.01, 250.0, Side::Sell));
    dispatch_update(place);
    ++stats.updates; ++stats.messages_dispatched;
    notify_tick(place.ts);

    // 3. Aggressive buy trades eating the density (5 lots at a time).
    int64_t t0 = 1'000'000'000LL;
    double remaining = 250.0;
    for (int step = 0; step < 30 && remaining > 5.0; ++step) {
        Trade t;
        t.price = 100.01;
        t.size  = 5.0;
        t.side  = Side::Buy;
        t.timestamp = ns_offset(t0 + step * 50'000'000LL);
        dispatch_trade(t);
        ++stats.trades; ++stats.messages_dispatched;

        remaining -= 5.0;
        OrderBookUpdate u;
        u.ticker = ticker_;
        u.ts = t.timestamp;
        u.changes.push_back(lvl(100.01, remaining, Side::Sell));
        dispatch_update(u);
        ++stats.updates; ++stats.messages_dispatched;
        notify_tick(u.ts);
    }

    // 4. Density gone — replaced with thin ask at 100.02 and a breakout trade.
    OrderBookUpdate erase;
    erase.ticker = ticker_;
    erase.ts = ns_offset(t0 + 30 * 50'000'000LL);
    erase.changes.push_back(lvl(100.01, 0.0, Side::Sell));
    erase.changes.push_back(lvl(100.02, 5.0, Side::Sell));
    dispatch_update(erase);
    ++stats.updates; ++stats.messages_dispatched;
    notify_tick(erase.ts);

    Trade breakout;
    breakout.price = 100.02;
    breakout.size  = 10.0;
    breakout.side  = Side::Buy;
    breakout.timestamp = ns_offset(t0 + 31 * 50'000'000LL);
    dispatch_trade(breakout);
    ++stats.trades; ++stats.messages_dispatched;
    notify_tick(breakout.timestamp);

    // 5. Cooldown ticks.
    for (int step = 0; step < 20; ++step) {
        notify_tick(ns_offset(t0 + (32 + step) * 50'000'000LL));
    }
}

// ── Scenario: leader_moves_alt_lags ──────────────────────────────────────────
//
// Two tickers (current ticker + leader) — leader price jumps, follower lags.
// V1 limitation: SyntheticFeed operates on a single ticker, so we only simulate
// the follower; leader correlation comes from the static feature path.  The
// scenario still exercises FeatureExtractor + LeaderSignal evaluation path.
//
void SyntheticFeed::run_leader_moves_alt_lags(RunStats& stats) {
    OrderBookSnapshot snap;
    snap.ticker = ticker_;
    snap.ts = ns_offset(0);
    for (int i = 0; i < 10; ++i) {
        snap.bids.push_back(lvl(50.00 - i * 0.01, 5.0, Side::Buy));
        snap.asks.push_back(lvl(50.01 + i * 0.01, 5.0, Side::Sell));
    }
    dispatch_snapshot(snap);
    ++stats.snapshots; ++stats.messages_dispatched;
    notify_tick(snap.ts);

    // Drift follower upward by small steps to simulate "lagging" behind a
    // leader that has already moved.  Each step: one update + one trade.
    for (int step = 1; step <= 30; ++step) {
        double bump = step * 0.005;
        OrderBookUpdate u;
        u.ticker = ticker_;
        u.ts = ns_offset(step * 100'000'000LL);
        u.changes.push_back(lvl(50.00 + bump, 5.0, Side::Buy));
        u.changes.push_back(lvl(50.01 + bump, 5.0, Side::Sell));
        dispatch_update(u);
        ++stats.updates; ++stats.messages_dispatched;

        Trade t;
        t.price = 50.01 + bump;
        t.size  = 2.0;
        t.side  = Side::Buy;
        t.timestamp = u.ts;
        dispatch_trade(t);
        ++stats.trades; ++stats.messages_dispatched;
        notify_tick(u.ts);
    }
}

void SyntheticFeed::run_risk_limit_rejection(RunStats& stats, ProbePipeline* pipeline) {
    if (!pipeline) return;

    // 1. Initial snapshot to set mid price, spread, etc.
    OrderBookSnapshot snap;
    snap.ticker = ticker_;
    snap.ts = ns_offset(0);
    // Simple symmetric levels around 100.0
    for (int i = 0; i < 10; ++i) {
        snap.bids.push_back(lvl(100.00 - i * 0.01, 10.0, Side::Buy));
        snap.asks.push_back(lvl(100.01 + i * 0.01, 10.0, Side::Sell));
    }
    dispatch_snapshot(snap);
    ++stats.snapshots; ++stats.messages_dispatched;
    notify_tick(snap.ts);

    // 2. Instantiate and register the mock strategy
    auto mock_strat = std::make_unique<MockRiskTestStrategy>(ticker_);
    auto* mock_strat_ptr = mock_strat.get();
    pipeline->strategy_engine().add_strategy(std::move(mock_strat));

    // Save initial account state
    const auto orig_account = pipeline->account();

    // Trace Id buffer
    uint64_t next_trace_id = 999001;

    // Helper to run a test case
    auto run_test_case = [&](const std::string& name, const std::function<void(TradePlan&, AccountState&)>& setup_fn) {
        // Reset account to clean defaults
        pipeline->account() = orig_account;

        // Construct standard valid baseline plan
        TradePlan plan;
        plan.ticker = ticker_;
        plan.side = Side::Buy;
        plan.entry_type = OrderType::Limit;
        plan.entry_price = 100.00;
        plan.stop_price = 99.90; // stop distance = 10 bps
        plan.tp1_price = 100.10; // TP distance = 10 bps (1.0 R:R)
        plan.tp1_size_ratio = 0.5;
        plan.size_coin = 1.0;
        plan.strategy_name = "MockRiskTestStrategy";
        plan.reason.assign("Risk Test Case: " + name);
        plan.trace_id = next_trace_id++;
        plan.valid_until = ns_offset(10'000'000'000LL); // 10s valid

        // Apply setup adjustments
        setup_fn(plan, pipeline->account());

        // Set the plan on mock strategy
        mock_strat_ptr->set_next_plan(plan);

        // Publish a dummy signal to trigger the strategy
        Signal sig;
        sig.ticker = ticker_;
        sig.kind = SignalKind::LevelApproach;
        sig.price = 100.0;
        sig.trigger_trace_id = plan.trace_id;
        sig.timestamp = ns_offset(0);
        sig.payload.touches = 1;
        sig.payload.speed_bps = 5.0;
        sig.payload.original_size = 10.0;
        sig.payload.size = 10.0;
        sig.payload.side = "Bid";

        // Dispatch signal through signal bus
        pipeline->signal_bus().publish(sig);

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
    run_test_case("StopTooTight", [](TradePlan& plan, AccountState&) {
        plan.stop_price = 99.98;
    });

    // Test Case 5: R6 Stop Too Wide
    run_test_case("StopTooWide", [](TradePlan& plan, AccountState&) {
        plan.stop_price = 99.00;
    });

    // Test Case 6: R7 TP1 R:R Too Low
    run_test_case("PoorRewardRisk", [](TradePlan& plan, AccountState&) {
        plan.tp1_price = 100.01;
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
    pipeline->account() = orig_account;
    pipeline->strategy_engine().remove_strategy(ticker_, "MockRiskTestStrategy");
}

} // namespace trade_bot::probe
