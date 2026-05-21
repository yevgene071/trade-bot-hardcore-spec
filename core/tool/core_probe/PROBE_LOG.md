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
| RiskManager | ❓ | — | — |
| PaperExecutor | ✅ | — | — |

## Session History
27: ### Session 2026-05-21 — Replay Timestamp Alignment & Trace ID Propagation
28: - **Objective**: Synchronize orderbook updates with historical trades in MetaScalp replay dumps; ensure matching trace IDs across trades and features; report correct market duration.
29: - **Implementation**:
30:   - Scanned first 1000 lines of NDJSON in `ReplayFeed` to calculate offset `trade_ts - recv_ts`.
31:   - Overrode orderbook snapshot and update timestamps with this offset during dispatch.
32:   - Tracked session start/end boundary times in `ProbePipeline` and recorded duration based on virtual clock.
33:   - Propagated active trace context `trace_id` to trade events in `on_trade`.
34: - **Results**:
35:   - ZEC_USDT replay correctly outputs matching trace IDs for trades and feature frame ticks.
36:   - Summary blocks report the correct duration (`6m 02s of market time`).
37:   - All synthetic scenarios run cleanly.

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
