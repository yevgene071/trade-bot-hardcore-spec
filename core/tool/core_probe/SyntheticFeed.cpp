#include "SyntheticFeed.hpp"
#include "perf/TraceContext.hpp"

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
    };
}

bool SyntheticFeed::is_known_scenario(const std::string& name) {
    auto all = available_scenarios();
    return std::find(all.begin(), all.end(), name) != all.end();
}

SyntheticFeed::RunStats SyntheticFeed::run() {
    RunStats stats;
    if (scenario_ == "density_appears") {
        run_density_appears(stats);
    } else if (scenario_ == "density_eaten_then_breakout") {
        run_density_eaten_then_breakout(stats);
    } else if (scenario_ == "leader_moves_alt_lags") {
        run_leader_moves_alt_lags(stats);
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

} // namespace trade_bot::probe
