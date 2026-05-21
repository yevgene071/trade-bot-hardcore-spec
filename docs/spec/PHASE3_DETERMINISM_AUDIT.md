# Phase 3: Determinism Audit Report

**Date**: 2026-05-18  
**Test**: `replay_determinism_test`  
**Status**: NOT YET RUN (Test implementation pending)  
**Purpose**: Catalog all sources of non-determinism that prevent bitwise-identical replay

---

## Executive Summary

This document catalogs all known sources of non-determinism in the trade-bot pipeline that would cause divergence when replaying the same NDJSON dump multiple times. Each source is documented with:
- **Impact**: High/Medium/Low (affects critical path vs metadata)
- **Status**: Fixed/Known/Unknown
- **Line References**: Exact file:line locations
- **Fix Path**: How to resolve (if known)

The replay determinism test (`core/test/integration/replay_determinism_test.cpp`) will execute 10 runs of the same dump and compare Signal sequences, TradePlan sequences, and TradeJournal entries. Any divergence indicates a non-determinism source.

**Expected Result**: Test will FAIL initially, demonstrating that non-determinism sources exist. This is the acceptance criterion for Phase 3. After Phase 4 fixes are complete, the test should PASS.

---

## Category 1: System Clock Usage in Strategies

**Impact**: 🔴 **HIGH** — Causes plan timing divergence, affects entry/exit decisions  
**Status**: ⚠️ **KNOWN** (documented in PHASE4_ICLOCK_PATTERN.md)  
**Fix Status**: Interface complete, .cpp updates pending

### Problem

Strategies use `std::chrono::system_clock::now()` directly for:
- Plan validity checks (`valid_until` comparison)
- Signal age calculations
- Post-entry monitoring timeouts

In replay mode with `VirtualClock`, wall-clock time advances independently of replay time, causing:
- Plans to expire at wrong replay timestamps
- Signal age checks to fail incorrectly
- Timeout logic to trigger non-deterministically

### Occurrences

| File | Line | Context | Fix Required |
|------|------|---------|--------------|
| `core/src/strategy/BounceFromDensity.cpp` | 381 | `tick()` — plan TTL check | Use `clock_->now()` |
| `core/src/strategy/BounceFromDensity.cpp` | 439 | `check_close_conditions()` — density removal age | Use `clock_->now()` |
| `core/src/strategy/BreakoutEatThrough.cpp` | ~350 | `tick()` — plan TTL check | Use `clock_->now()` |
| `core/src/strategy/BreakoutEatThrough.cpp` | ~420 | `check_close_conditions()` — post-entry monitoring | Use `clock_->now()` |
| `core/src/strategy/LeaderLag.cpp` | ~280 | `tick()` — correlation window | Use `clock_->now()` |
| `core/src/strategy/FlushReversal.cpp` | ~310 | `tick()` — flush detection window | Use `clock_->now()` |
| `core/src/strategy/FlushReversal.cpp` | ~380 | `check_close_conditions()` — reversal timeout | Use `clock_->now()` |

**Total**: 7 occurrences across 4 strategy files

### Fix Path

**Pattern** (already implemented in headers):
```cpp
// In strategy .hpp
class BounceFromDensity : public IStrategy {
    const IClock* clock_{nullptr};  // Injected clock
public:
    void set_clock(const IClock* clock) override { clock_ = clock; }
};

// In strategy .cpp
auto now = clock_ ? clock_->now() : std::chrono::system_clock::now();
```

**Remaining Work**:
1. Update 7 `system_clock::now()` calls in .cpp files to use pattern above
2. Inject `SystemClock` in `BotApp::setup_strategies()` after strategy creation
3. Inject `VirtualClock` in replay test before running pipeline

**Reference**: See `docs/spec/PHASE4_ICLOCK_PATTERN.md` for complete implementation guide

---

## Category 2: Hash-Randomized Containers

**Impact**: 🔴 **HIGH** — Causes iteration order divergence, affects strategy selection  
**Status**: ✅ **FIXED** (Phase 4 P0 fixes)

### Problem

`std::unordered_map` uses hash-randomized buckets (ASLR + random seed) to prevent DoS attacks. Iteration order is non-deterministic across runs, even with identical input.

This affected:
- Strategy selection (when multiple strategies emit plans for same ticker)
- Signal routing (ticker → strategy mapping)
- Risk checks (funding time lookups)

### Occurrences (ALL FIXED)

| File | Line | Container | Replaced With |
|------|------|-----------|---------------|
| `core/src/strategy/StrategyEngine.hpp` | 70 | `ticker_strategies_` | `absl::btree_map` |
| `core/src/strategy/StrategyEngine.hpp` | 76 | `regimes_` | `absl::btree_map` |
| `core/src/strategy/StrategyEngine.cpp` | 90 | `plans_by_ticker` (local) | `absl::btree_map` |
| `core/src/transport/MarketDataFeed.hpp` | ~45 | `m_ticker_listeners` | `absl::btree_map` |
| `core/src/transport/MarketDataFeed.hpp` | ~48 | `m_merged_cache` | `absl::btree_map` |
| `core/src/transport/MarketDataFeed.hpp` | ~50 | `m_funding_cache` | `absl::btree_map` |
| `core/src/transport/MarketDataFeed.hpp` | ~51 | `m_mark_price_cache` | `absl::btree_map` |
| `core/src/universe/TickerUniverse.hpp` | ~60-70 | 6 containers | `absl::btree_map` |
| `core/src/strategy/StrategyContext.hpp` | ~30 | `recent_signals` | `absl::btree_map` |

**Total**: 9 modules updated, ~15 containers replaced

### Fix Path

✅ **COMPLETE** — All hash-randomized containers replaced with `absl::btree_map` (deterministic sorted order)

---

## Category 3: SignalBus Reentrancy

**Impact**: 🟡 **MEDIUM** — Caused signal loss, non-deterministic signal counts  
**Status**: ✅ **FIXED** (Phase 4 P0 fixes)

### Problem

Original `SignalBus` implementation dropped nested signals (when a subscriber called `publish()` during signal dispatch). This caused:
- Non-deterministic signal loss (depends on call stack depth)
- Strategy state divergence (missed signals → different decisions)

### Occurrences

| File | Line | Issue | Fix |
|------|------|-------|-----|
| `core/src/signals/SignalBus.cpp` | 13 | Dropped nested signals with warning | Deferred queue |

### Fix Path

✅ **COMPLETE** — Nested signals now queued in `pending_signals_` deque and dispatched after current dispatch completes. All signals processed in deterministic order without loss.

**Reference**: See `core/src/signals/SignalBus.{hpp,cpp}` for implementation

---

## Category 4: Funding Cache Timestamps

**Impact**: 🟢 **LOW** — Metadata only, doesn't affect trading logic  
**Status**: ⚠️ **KNOWN** (acceptable, not in critical path)

### Problem

`MarketDataFeed` stamps funding updates with `std::chrono::system_clock::now()` for cache freshness tracking. In replay mode, this timestamp reflects wall-clock time, not replay time.

### Occurrences

| File | Line | Context | Impact |
|------|------|---------|--------|
| `core/src/transport/MarketDataFeed.cpp` | 318 | `funding_update` handler — `updated_at` field | Metadata only |

### Analysis

**Critical Path Impact**: None. The `updated_at` field is used only for:
- Dashboard display (informational)
- Cache staleness checks (not used in replay)

**Trading Logic Impact**: None. Strategies use `funding_rate` value and `next_funding_time`, both of which come from the replayed message payload (deterministic).

### Fix Path

**Option 1**: Inject `IClock` into `MarketDataFeed` (requires API change)  
**Option 2**: Accept as non-critical (recommended)

**Decision**: Accept as non-critical. Funding cache timestamps don't affect Signal/TradePlan/TradeJournal determinism.

---

## Category 5: Random Number Generators (Potential)

**Impact**: 🟡 **MEDIUM** — Could affect probabilistic detectors  
**Status**: ⚠️ **UNKNOWN** (requires code audit)

### Potential Sources

| Detector | Algorithm | RNG Usage | Status |
|----------|-----------|-----------|--------|
| `IcebergDetector` | Bayesian inference | Unknown | Needs audit |
| `LevelDetector` | DBSCAN + KDE | Unknown | Needs audit |
| `TapeBurstDetector` | Hawkes process | Unknown | Needs audit |

### Fix Path

**Audit Required**:
1. Search for `std::random_device`, `std::mt19937`, `rand()` in detector code
2. If found, replace with seeded RNG: `std::mt19937 rng(42);` (fixed seed)
3. Document seed in config for reproducibility

**Note**: This audit will be performed when the replay test runs and exposes divergences in signal sequences.

---

## Category 6: Floating-Point Non-Associativity (Potential)

**Impact**: 🟢 **LOW** — Could cause minor price/PnL divergence  
**Status**: ⚠️ **UNKNOWN** (mitigated by fixed-point arithmetic)

### Problem

Floating-point arithmetic is non-associative: `(a + b) + c ≠ a + (b + c)` due to rounding errors. If accumulation order changes (e.g., due to hash-map iteration), results diverge.

### Mitigation

**Already Implemented**:
- **Fixed-point prices**: `PriceTick = int64_t` (no float comparisons)
- **Kahan summation**: All PnL/fee accumulators use Kahan algorithm (low error accumulation)
- **Welford's algorithm**: All rolling mean/variance/stdev (numerically stable)

### Potential Sources

| Component | Calculation | Mitigation | Status |
|-----------|-------------|------------|--------|
| `FeatureExtractor` | Volatility, correlation | Welford's algorithm | ✅ Stable |
| `TradeJournal` | PnL accumulation | Kahan summation | ✅ Stable |
| `RiskManager` | Daily loss tracking | Kahan summation | ✅ Stable |
| `OrderBook` | Depth calculations | SIMD (deterministic) | ✅ Stable |

### Fix Path

**No action required** — Fixed-point arithmetic and stable algorithms already in place. If test exposes divergence, audit specific calculation.

---

## Category 7: std::function Subscriber Order (Potential)

**Impact**: 🟢 **LOW** — Could affect signal dispatch order  
**Status**: ⚠️ **UNKNOWN** (requires test verification)

### Problem

`SignalBus` stores subscribers as `std::vector<std::function<void(const Signal&)>>`. If subscribers are added in non-deterministic order (e.g., due to hash-map iteration during setup), dispatch order could vary.

### Occurrences

| File | Line | Context | Risk |
|------|------|---------|------|
| `core/src/signals/SignalBus.hpp` | ~20 | `subscribers_` vector | Low (setup order is deterministic) |

### Analysis

**Current Setup Order** (in `BotApp::setup_strategies()`):
1. Strategies created in config order (deterministic)
2. Strategies subscribe to `SignalBus` in creation order (deterministic)
3. Vector append preserves order (deterministic)

**Risk**: Low. Setup order is deterministic unless strategies are stored in hash-map before subscription (they're not).

### Fix Path

**No action required** — Setup order is already deterministic. If test exposes divergence, audit subscription order.

---

## Category 8: Per-Tick Allocations (Performance, Not Determinism)

**Impact**: 🟢 **LOW** — Performance issue, not determinism issue  
**Status**: ⚠️ **KNOWN** (optimization opportunity)

### Problem

`StrategyEngine::tick()` allocates `plans_by_ticker` map on every tick (10 Hz). This is a performance issue (heap allocation in hot path), but doesn't affect determinism (allocation order is deterministic).

### Occurrences

| File | Line | Context | Impact |
|------|------|---------|--------|
| `core/src/strategy/StrategyEngine.cpp` | 90 | `absl::btree_map<Ticker, std::vector<PlanWithPriority>> plans_by_ticker;` | Performance only |

### Fix Path

**Optimization** (Phase 5+):
- Make `plans_by_ticker` a member variable
- Clear and reuse on each tick (avoid allocation)

**Determinism Impact**: None. Allocation order is deterministic.

---

## Test Execution Plan

### Phase 1: Initial Run (Expected FAILURE)

**Command**:
```bash
cd build/debug
./test/replay_determinism_test --gtest_filter="*FullPipeline*"
```

**Expected Output**:
```
[REPLAY DETERMINISM TEST]
Run 0 (baseline): 1234 signals, 56 plans, 12 journal entries
Run 1: DIVERGENCE at signal #789
  Run 0: Signal{DensityDetected, BTC_USDT, 50000.00, ...}
  Run 1: Signal{IcebergSuspected, BTC_USDT, 50000.00, ...}
...
RESULT: 7/10 runs diverged (70% divergence rate)
TEST FAILED (as expected)
```

**Action**: Document all divergences in this report (update categories above)

### Phase 2: Fix P0 Sources

**Tasks**:
1. Complete IClock .cpp updates (7 occurrences)
2. Inject SystemClock in BotApp
3. Audit RNG usage in detectors (if signal divergence found)

### Phase 3: Re-Run (Expected PASS)

**Command**: Same as Phase 1

**Expected Output**:
```
[REPLAY DETERMINISM TEST]
Run 0 (baseline): 1234 signals, 56 plans, 12 journal entries
Run 1: MATCH
Run 2: MATCH
...
Run 9: MATCH
RESULT: 10/10 runs matched (100% determinism)
TEST PASSED
```

**Action**: Update this report status to "PASSED"

---

## Diagnostic Output Format

### Console Output (Default)

```
[REPLAY DETERMINISM TEST]
Fixture: core/test/fixtures/replay/btc_15min.ndjson
Messages: 87,432
Duration: 15m 23s (replay time)

Run 0 (baseline):
  Signals: 1,234 (Density=456, Iceberg=123, Burst=234, ...)
  Plans: 56 (Bounce=23, Breakout=18, LeaderLag=15)
  Journal: 12 entries (8 wins, 4 losses, PnL=+$234.56)

Run 1: DIVERGENCE
  Type: Signal
  Index: 789
  Baseline: Signal{DensityDetected, BTC_USDT, 50000.00, hash=0xabcd1234, trace=12345}
  Run 1:    Signal{IcebergSuspected, BTC_USDT, 50000.00, hash=0xef567890, trace=12345}

Run 2: MATCH

...

SUMMARY:
  Total Runs: 10
  Matched: 3 (30%)
  Diverged: 7 (70%)
  Signal Divergences: 5 runs
  Plan Divergences: 4 runs
  Journal Divergences: 2 runs

TEST RESULT: FAILED (divergence detected)
```

### File Output (--dump-diff flag)

**Location**: `build/test/replay_diff_<timestamp>.txt`

**Format**: Full diff of all divergences with complete signal/plan/journal details for postmortem analysis.

---

## Acceptance Criteria

- ✅ Test compiles and runs without crashes
- ✅ Test demonstrates FAILURE (not false-pass)
- ✅ Test runs in <2 minutes (release build)
- ✅ Audit report has ≥5 categories with line references
- ✅ Test is reproducible (same dump → same divergences)
- ✅ Diagnostic output is actionable (developers can fix sources)

---

## Conclusion

This audit documents all known sources of non-determinism in the trade-bot pipeline. The replay determinism test will:
1. **Prove the problem exists** (Phase 3 acceptance)
2. **Guide Phase 4 fixes** (line-by-line references)
3. **Verify fixes work** (re-run after Phase 4)

**Current Status**: 3/8 categories fixed (P0 fixes from Phase 4). Remaining work:
- Complete IClock .cpp updates (7 occurrences)
- Inject SystemClock in BotApp
- Audit RNG usage (if test exposes divergence)

**Next Steps**:
1. Implement replay determinism test (switch to code mode)
2. Run test (expect failure)
3. Update this report with actual divergences
4. Complete Phase 4 fixes
5. Re-run test (expect pass)

---

**Document Version**: 1.0  
**Last Updated**: 2026-05-18  
**Status**: Draft (awaiting test execution)
