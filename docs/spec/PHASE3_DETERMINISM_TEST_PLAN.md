# Phase 3: Replay Determinism Test — Implementation Plan

**Status**: Design Complete  
**Date**: 2026-05-18  
**Author**: Bob Shell (Plan Mode)

## Overview

This document specifies the implementation of a full-pipeline replay determinism test that will:
1. Prove deterministic replay capability (or expose remaining non-determinism)
2. Catalog all sources of divergence with line-by-line references
3. Serve as acceptance gate for Phase 3 completion

## Test Architecture

### Pipeline Wiring

```
ReplayFeed (VirtualClock)
    ↓
MarketDataFeed (listener routing)
    ↓
TickerController (OrderBook + TradeStream + FeatureExtractor)
    ↓
SignalBus (7 detectors subscribed)
    ↓
StrategyEngine (4 strategies)
    ↓
RiskManager (15 rules)
    ↓
PaperExecutor (no network, deterministic fills)
    ↓
TradeJournal (in-memory accumulation)
```

### Key Design Decisions

1. **Isolated Components**: Create all components from scratch per run (no shared state)
2. **VirtualClock Injection**: Use `VirtualClock` for all time-dependent logic
3. **PaperExecutor Only**: Avoid `LiveExecutor` (has exchange response non-determinism)
4. **In-Memory Journal**: Accumulate all entries in memory for comparison
5. **Signal History**: Capture all signals with full payload for comparison
6. **N=10 Runs**: Run same dump 10 times, compare each to run #0 (baseline)

## Test Fixtures

### Option A: Real 15-Minute BTC Dump (Preferred)

**Source**: Use existing `DumpRecorder` output from live session  
**Location**: `core/test/fixtures/replay/btc_15min.ndjson`  
**Requirements**:
- ~15 minutes of active BTC_USDT trading
- ~50,000-100,000 WebSocket messages
- Must include: orderbook snapshots, trades, funding updates, mark price updates
- Captured during volatile period (ensures signal triggers)

**Generation** (if no dump exists):
```bash
# Run bot with DumpRecorder enabled for 15 minutes
./build/release/bin/trade_bot --config config.toml --record-dump btc_15min.ndjson
# Stop after 15 minutes
# Move to test fixtures
mv btc_15min.ndjson core/test/fixtures/replay/
```

### Option B: Synthetic Deterministic Dump (Fallback)

**Generator**: `core/test/fixtures/replay/generate_synthetic_dump.cpp`  
**Characteristics**:
- Fixed RNG seed (42)
- Deterministic price walk (sine wave + noise)
- Controlled signal triggers (density, level, burst events)
- ~10,000 messages (faster test execution)

**Advantages**:
- No dependency on live market data
- Reproducible across environments
- Faster test execution (<30s vs ~2min)

**Disadvantages**:
- May not expose real-world edge cases
- Synthetic patterns may not trigger all strategies

## Comparison Logic

### 1. Signal Sequence Comparison

```cpp
struct SignalFingerprint {
    SignalKind kind;
    Ticker ticker;
    PriceTick price_tick;  // rounded to tick_size
    uint64_t payload_hash;  // hash of all payload fields
    TraceId trigger_trace_id;
    
    bool operator==(const SignalFingerprint&) const = default;
};

std::vector<SignalFingerprint> extract_signal_sequence(const std::vector<Signal>&);
```

**Comparison**: Exact sequence match (order matters)  
**Divergence Output**: First mismatch index + both signals

### 2. TradePlan Sequence Comparison

```cpp
struct TradePlanFingerprint {
    Ticker ticker;
    Side side;
    PriceTick entry_tick;
    PriceTick stop_tick;
    PriceTick tp1_tick;
    std::string strategy_name;
    std::vector<TraceId> evidence_trace_ids;  // sorted
    
    bool operator==(const TradePlanFingerprint&) const = default;
};

std::vector<TradePlanFingerprint> extract_plan_sequence(const std::vector<TradePlan>&);
```

**Comparison**: Exact sequence match  
**Divergence Output**: First mismatch index + both plans

### 3. TradeJournal Entry Comparison

```cpp
struct JournalFingerprint {
    Ticker ticker;
    std::string strategy_name;
    Side side;
    PriceTick entry_tick;
    PriceTick exit_tick;
    int64_t pnl_usd_cents;  // KahanAccumulator result * 100
    std::string cause_of_exit;
    
    bool operator==(const JournalFingerprint&) const = default;
};

std::vector<JournalFingerprint> extract_journal_sequence(const std::vector<TradeJournal::Entry>&);
```

**Comparison**: Exact sequence match  
**Divergence Output**: First mismatch index + both entries

## Diagnostic Output

### Console Output (Default)

```
[REPLAY DETERMINISM TEST]
Run 0 (baseline): 1234 signals, 56 plans, 12 journal entries
Run 1: DIVERGENCE at signal #789
  Run 0: Signal{DensityDetected, BTC_USDT, 50000.00, hash=0xabcd1234, trace=12345}
  Run 1: Signal{IcebergSuspected, BTC_USDT, 50000.00, hash=0xef567890, trace=12345}
Run 2: MATCH
Run 3: DIVERGENCE at plan #23
  Run 0: Plan{BounceFromDensity, BTC_USDT, Buy, entry=50001.00, stop=49990.00}
  Run 1: Plan{BreakoutEatThrough, BTC_USDT, Buy, entry=50002.00, stop=49985.00}
...
RESULT: 3/10 runs matched baseline (70% divergence rate)
```

### File Output (--dump-diff flag)

**Location**: `build/test/replay_diff_<timestamp>.txt`  
**Format**:
```
=== REPLAY DETERMINISM DIFF ===
Dump: core/test/fixtures/replay/btc_15min.ndjson
Runs: 10
Baseline: Run 0

--- Run 1 Divergence ---
Type: Signal
Index: 789
Baseline: Signal{kind=DensityDetected, ticker=BTC_USDT, price=50000.00, ...}
Run 1:    Signal{kind=IcebergSuspected, ticker=BTC_USDT, price=50000.00, ...}

--- Run 3 Divergence ---
Type: TradePlan
Index: 23
Baseline: Plan{strategy=BounceFromDensity, ticker=BTC_USDT, ...}
Run 3:    Plan{strategy=BreakoutEatThrough, ticker=BTC_USDT, ...}

=== SUMMARY ===
Total Runs: 10
Matched: 3 (30%)
Diverged: 7 (70%)
Signal Divergences: 5
Plan Divergences: 4
Journal Divergences: 2
```

## Implementation Steps

### Step 1: Test Fixture Preparation

**Files**:
- `core/test/fixtures/replay/btc_15min.ndjson` (real dump, preferred)
- `core/test/fixtures/replay/generate_synthetic_dump.cpp` (fallback generator)

**Acceptance**:
- At least one fixture available
- Fixture loads without parse errors
- Fixture triggers at least 3 different signal types

### Step 2: Comparison Infrastructure

**Files**:
- `core/test/integration/replay_determinism_test.cpp` (main test)
- Helper structs: `SignalFingerprint`, `TradePlanFingerprint`, `JournalFingerprint`
- Comparison functions: `compare_signals()`, `compare_plans()`, `compare_journals()`

**Acceptance**:
- Fingerprint extraction compiles and runs
- Comparison logic handles empty sequences
- Hash functions are deterministic (same input → same hash)

### Step 3: Pipeline Wiring

**Components to Create**:
```cpp
// Per-run isolated state
struct ReplayRun {
    std::shared_ptr<VirtualClock> clock;
    std::unique_ptr<ReplayFeed> feed;
    std::unique_ptr<OrderBook> book;
    std::unique_ptr<TradeStream> stream;
    std::unique_ptr<FeatureExtractor> extractor;
    std::unique_ptr<SignalBus> signal_bus;
    std::unique_ptr<StrategyEngine> strategy_engine;
    std::unique_ptr<RiskManager> risk_manager;
    std::unique_ptr<PaperExecutor> executor;
    std::unique_ptr<TradeJournal> journal;
    
    std::vector<Signal> captured_signals;
    std::vector<TradePlan> captured_plans;
};
```

**Wiring Logic**:
1. Create all components with VirtualClock
2. Subscribe signal capturer to SignalBus
3. Subscribe plan capturer to StrategyEngine
4. Wire ReplayFeed → OrderBook → FeatureExtractor → SignalBus → StrategyEngine → RiskManager → PaperExecutor
5. Run `feed->run()` until exhausted
6. Extract journal entries via `journal->get_all_entries()`

**Acceptance**:
- Pipeline runs without crashes
- At least 1 signal captured
- At least 1 plan generated (if fixture has sufficient activity)

### Step 4: Multi-Run Comparison

**Logic**:
```cpp
TEST(ReplayDeterminismTest, FullPipelineDeterminism) {
    const int N = 10;
    std::vector<ReplayRun> runs;
    
    for (int i = 0; i < N; ++i) {
        runs.push_back(run_replay("btc_15min.ndjson"));
    }
    
    // Compare each run to baseline (run 0)
    for (int i = 1; i < N; ++i) {
        auto sig_diff = compare_signals(runs[0].captured_signals, runs[i].captured_signals);
        auto plan_diff = compare_plans(runs[0].captured_plans, runs[i].captured_plans);
        auto journal_diff = compare_journals(runs[0].journal->get_all_entries(), 
                                             runs[i].journal->get_all_entries());
        
        if (sig_diff || plan_diff || journal_diff) {
            LOG_ERROR("Run {} diverged from baseline", i);
            // Print first divergence
        }
    }
}
```

**Acceptance**:
- Test compiles and runs
- Test completes in <2 minutes (release build)
- Test prints divergence details (if any)

### Step 5: Diagnostic Mode

**Flag**: `--dump-diff` (GTest filter: `--gtest_filter=*Determinism* --dump-diff`)  
**Implementation**: Check `argc`/`argv` in test, write to file if flag present

**Acceptance**:
- File written to `build/test/replay_diff_<timestamp>.txt`
- File contains all divergences with full details
- File is human-readable

## Expected Test Result

**CRITICAL**: This test is EXPECTED to FAIL initially. This is the acceptance criterion.

### Why Failure is Expected

The Phase 4 P0 fixes are incomplete:
1. **IClock pattern**: Interface exists, but 7 `system_clock::now()` calls remain in strategy .cpp files
2. **Strategies not injected with clock**: `BotApp` doesn't call `set_clock()` on strategies yet
3. **Potential remaining sources**: See PHASE3_DETERMINISM_AUDIT.md

### Acceptance Criteria

✅ **Test demonstrates FAILURE** — prints list of divergences  
✅ **Test does NOT false-pass** — actually runs full pipeline, not just message counting  
✅ **Test runs in <2 minutes** — release build, real or synthetic fixture  
✅ **Audit report populated** — ≥5 categories with line references  

### Success Path (Phase 4+)

After all P0 fixes are complete:
1. Complete IClock .cpp updates (7 occurrences)
2. Inject SystemClock in BotApp
3. Re-run test → should PASS (all runs match baseline)
4. This proves deterministic replay capability

## Audit Report Structure

**File**: `docs/spec/PHASE3_DETERMINISM_AUDIT.md`

### Template

```markdown
# Phase 3: Determinism Audit Report

**Date**: 2026-05-18  
**Test**: `replay_determinism_test`  
**Status**: FAILED (Expected)

## Executive Summary

Replay determinism test executed 10 runs of a 15-minute BTC_USDT dump.
Result: X/10 runs diverged from baseline.

## Divergence Categories

### 1. System Clock Usage in Strategies

**Impact**: High (causes plan timing divergence)  
**Status**: Known, documented in PHASE4_ICLOCK_PATTERN.md

**Occurrences**:
- `core/src/strategy/BounceFromDensity.cpp:381` — `std::chrono::system_clock::now()` in `tick()`
- `core/src/strategy/BounceFromDensity.cpp:439` — `std::chrono::system_clock::now()` in `check_close_conditions()`
- `core/src/strategy/BreakoutEatThrough.cpp:XXX` — (similar)
- `core/src/strategy/LeaderLag.cpp:XXX` — (similar)
- `core/src/strategy/FlushReversal.cpp:XXX` — (similar)

**Fix**: Use `clock_ ? clock_->now() : std::chrono::system_clock::now()` pattern

### 2. Hash-Randomized Containers (FIXED)

**Impact**: High (causes iteration order divergence)  
**Status**: FIXED in Phase 4

**Occurrences**: All replaced with `absl::btree_map` (9 modules)

### 3. SignalBus Reentrancy (FIXED)

**Impact**: Medium (caused signal loss)  
**Status**: FIXED in Phase 4 (deferred queue)

### 4. Funding Cache Timestamps

**Impact**: Low (metadata only, doesn't affect logic)  
**Status**: Acceptable (not in critical path)

**Occurrences**:
- `core/src/transport/MarketDataFeed.cpp:318` — `std::chrono::system_clock::now()` for `updated_at`

**Fix**: Use injected clock or accept as non-critical

### 5. [Additional categories as discovered]

## Test Output Sample

[Paste actual test output showing divergences]

## Recommendations

1. Complete IClock .cpp updates (7 occurrences)
2. Inject SystemClock in BotApp after strategy creation
3. Re-run test to verify fixes
4. Consider additional sources if divergence persists

## Conclusion

Test successfully demonstrates non-determinism sources.
All P0 sources are documented and have clear fix paths.
Phase 4 completion will enable deterministic replay.
```

## Out of Scope

1. **Fixes themselves** — Phase 4 task
2. **LiveExecutor determinism** — exchange responses are inherently non-deterministic
3. **Network timing** — replay uses VirtualClock, no real network
4. **ML/Phase 5 features** — not yet implemented

## Success Metrics

- ✅ Test compiles and runs
- ✅ Test demonstrates FAILURE (not false-pass)
- ✅ Test runs in <2 minutes
- ✅ Audit report has ≥5 categories with line references
- ✅ Test is reproducible (same dump → same divergences)
- ✅ Diagnostic output is actionable (developers can fix sources)

## Next Steps

1. **Implement test** (switch to code mode)
2. **Run test** (expect failure)
3. **Populate audit report** (document all divergences)
4. **Phase 4 fixes** (separate task)
5. **Re-run test** (expect pass after fixes)

---

**End of Plan**
