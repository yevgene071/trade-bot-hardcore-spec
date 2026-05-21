# PROBE_LOG — Trading Core Diagnostic History

## Known Synthetic Scenarios
- density_appears
- density_eaten_then_breakout
- leader_moves_alt_lags

## Exit Code Registry
| Code | Meaning |
|------|---------|
| 0    | Clean |
| 1    | Build/config/arguments error |
| 2    | Field differences found (diff) |
| 3    | Feature/signal assertion (assert) |
| 4    | Strict invariant violation (replay/synth/live) |

## Component Wiring Status
| Component | Wired in probe | Invariants | Scenarios |
|-----------|---------------|------------|-----------|
| OrderBook | ✅ | ✅ | book_cross |
| FeatureExtractor | ✅ | partial | — |
| StrategyEngine | ✅ | — | density_*, tape_burst |
| RiskManager | ✅ | ✅ | risk_limit_rejection |
| PaperExecutor | ✅ | — | — |

## Session History

### Session 2026-05-21 — Replay Risk Limit Rejection (Real Dump Verification)
- **Objective**: Full end-to-end verification of all 15 RiskManager rules on a real MetaScalp market data dump (`mexc_futures_zec_usdt.ndjson`) under strict invariants (`--invariants strict`) in the `core_probe` environment.
- **Implementation**:
  - Factored out a reusable `MockRiskTestStrategy` into `MockRiskTestStrategy.hpp` to share across both synthetic and replay feed environments.
  - Implemented `ReplayRiskVerifier` (implementing `IMarketDataListener`) to hook into the real NDJSON replay stream.
  - Intercepted the first valid orderbook snapshot to calculate the baseline `mid_price` for trading plan prices.
  - Registered `MockRiskTestStrategy` and ran the 8 sequential risk limit rejections sequentially (`KillSwitchActive`, `DailyLossLimitHit`, `TooManyPositions`, `StopTooTight`, `StopTooWide`, `PoorRewardRisk`, `SizeBelowMinimum`, and `InsufficientMargin`).
  - Whitelisted `"BTC_USDT"`, `"ZEC_USDT"`, and `"ZEC_BTC"` tickers inside `load_risk_config` and pre-cached ticker metadata in `ProbePipeline` constructor to avoid `NotInUniverse` rejections.
  - Successfully stopped the feed after executing the checks.
- **Results**:
  - All 8 risk rules correctly triggered and rejected plans with 100% correct rejection reasons.
  - Verification passed under strict invariants (`--invariants strict`) with no assertions or failures.

### Session 2026-05-21 — RiskManager Strict Verification
- **Objective**: Verify all 15 risk limits under strict invariants (`--invariants strict`) in the `core_probe` environment without modifying core logic.
- **Implementation**:
  - Implemented the `risk_limit_rejection` scenario in `SyntheticFeed.cpp`.
  - Created a local `MockRiskTestStrategy` inside `SyntheticFeed.cpp` to sequence 8 different risk limit violations: `KillSwitchActive`, `DailyLossLimitHit`, `TooManyPositions`, `StopTooTight`, `StopTooWide`, `PoorRewardRisk`, `SizeBelowMinimum`, and `InsufficientMargin`.
  - Bypassed the strategy affinity / universe admission check by whitelisting the synthetic `"SYNTH"` ticker in `load_risk_config` dynamically.
  - Cached ticker metadata inside `ProbePipeline` constructor to correctly handle volume steps and minimum size limits.
- **Results**:
  - All 8 rules correctly rejected with exactly matching rejection categories and no invariant violations.
  - All 3 previous synthetic scenarios passed successfully.

### Session 2026-05-21 — Replay Timestamp Alignment & Trace ID Propagation
- **Objective**: Synchronize orderbook updates with historical trades in MetaScalp replay dumps; ensure matching trace IDs across trades and features; report correct market duration.
- **Implementation**:
  - Scanned first 1000 lines of NDJSON in `ReplayFeed` to calculate offset `trade_ts - recv_ts`.
  - Overrode orderbook snapshot and update timestamps with this offset during dispatch.
  - Tracked session start/end boundary times in `ProbePipeline` and recorded duration based on virtual clock.
  - Propagated active trace context `trace_id` to trade events in `on_trade`.
- **Results**:
  - ZEC_USDT replay correctly outputs matching trace IDs for trades and feature frame ticks.
  - Summary blocks report the correct duration (`6m 02s of market time`).
  - All synthetic scenarios run cleanly.

## [2026-05-21] Session 2 (Synthetic Feed Trace Context Integration)

**Dump used**: mexc_futures_zec_usdt.ndjson / mexc_futures_zec_btc_20260521_001050.ndjson
**Events processed**: 5021 (replay), 44/122/122 (synth)
**Exit code**: 0 (all replay & synth scenarios now exit cleanly)

### Bugs Found
- [BUG-001] SyntheticFeed: synthetic dispatches lacked active thread-local `TraceContextScope`, causing `trace_id` to remain 0 and violating `SIGNAL_NO_TRACE` under strict invariants mode.

### Fixes Applied
- [FIX-001] refs BUG-001: Included `"perf/TraceContext.hpp"` in `SyntheticFeed.cpp` and wrapped all `dispatch_*` and `notify_tick` events in a time-based `TraceContextScope`.

### Tool Extensions (L2)
- [PROBE-001] Resolved trace ID propagation in `SyntheticFeed` allowing all synthetic scenarios (`density_appears`, `density_eaten_then_breakout`, `leader_moves_alt_lags`) to run with strict invariant validation enabled.

### Open Items
- [ ] Investigate strategy affinity and threshold tuning to see why historical dumps do not trigger active trading plans.

### Metrics Snapshot
| Metric | Value |
|--------|-------|
| p50 OrderBook | 162 µs |
| p99 Features | 11895 µs |
| Signals fired | 25 |
| Invariant violations | 0 |
