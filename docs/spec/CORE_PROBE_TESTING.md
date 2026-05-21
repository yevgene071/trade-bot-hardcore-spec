# Core Probe Testing & Verification

`core_probe` is a high-performance diagnostic, simulation, and tracing harness designed to test the **entire trading core** (order book reconstruction, feature engineering, signal detection, risk management, and execution engines) in a deterministic sandboxed environment.

This document serves as a **formal skill specification** for other AI agents to execute end-to-end integration tests, replay real production market dumps, and continuously expand core test coverage.

---

## 1. Tool Overview & Architecture

Unlike simple isolated unit tests, `core_probe` replicates the complete live application environment, substituting only the unstable external network transport with deterministic historical log streams (`ReplayFeed`) or synthetic scenarios (`SynthFeed`).

```
                    ┌─────────────────────────┐
                    │  Market Dump (.ndjson)  │
                    └────────────┬────────────┘
                                 │
                                 ▼
                     ┌───────────────────────┐
                     │      ReplayFeed       │
                     └───────────┬───────────┘
                                 │ (Decoded MetaScalp API events)
                                 ▼
┌────────────────────────────────────────────────────────────────────────┐
│                        TRADING ENGINE (Sandbox)                        │
│                                                                        │
│   ┌───────────────────────┐         ┌──────────────────────────────┐   │
│   │     OrderBook (2)     ├────────►│   FeatureExtractor (SIMD)    │   │
│   └───────────────────────┘         └──────────────┬───────────────┘   │
│                                                    │                   │
│                                                    ▼                   │
│   ┌───────────────────────┐         ┌──────────────────────────────┐   │
│   │     PaperExecutor     │◄────────┤        StrategyEngine        │   │
│   └───────────────────────┘         └──────────────▲───────────────┘   │
│               ▲                                    │                   │
│               │                                    │ (Signals)         │
│               │ (Orders)                           │                   │
│   ┌───────────┴───────────┐         ┌──────────────┴───────────────┐   │
│   │      RiskManager      │◄────────┤        7 Detectors           │   │
│   └───────────────────────┘         └──────────────────────────────┘   │
└────────────────────────────────────────────────────────────────────────┘
```

The tool is isolated, meaning **zero actual trades are executed on the exchange**, yet the entire business logic behaves exactly as it would in production.

---

## 2. Command Reference

### Running Replays on Real Production Dumps
To run the full core pipeline over a raw NDJSON production dump (located in `replay/dumps/`):
```bash
./build/debug/bin/core_probe replay \
    --dump replay/dumps/dump_2026-05-18T12-16-29.ndjson \
    --ticker BTC_USDT \
    --limit 10000
```
* **`--dump <path>`**: Path to the historical feed. Real dumps contain interleaved packets from multiple tickers.
* **`--ticker <symbol>`**: Registers the specified controller in the pipeline. All other symbols in the dump are skipped dynamically.
* **`--limit <n>`**: Halts execution instantly after reading `n` matching messages (highly useful for fast regression checking).

### Running Synthetic Scenarios
To run test scenarios designed to trigger specific state machine states (e.g., density appearing or disappearing):
```bash
./build/debug/bin/core_probe synth --scenario density_appears
```

---

## 3. What is Tested & Verified

Every `core_probe` run evaluates the core against strict benchmarks and reports metrics:

1. **Deterministic OrderBook Reconstruction**:
   * Rebuilds Bid/Ask levels in lockstep from raw JSON `orderbook_update` events.
   * Tests the case-insensitive field matching and `BestBid`/`BestAsk` edge cases.
2. **Feature Frame Consistency**:
   * Evaluates rolling standard deviation, imbalance (`imb`), and spread in real-time.
   * Compares hot-path SIMD performance metrics.
3. **Signal Detection Sensitivity**:
   * Triggers detectors such as `TapeBurst` or `DensityRemoved` on real-world volatility.
   * Reports final counts of generated signals.
4. **Performance & Latency**:
   * Measures `p50`, `p90`, `p99`, `p99.9` latency in microseconds for every hot-path phase.
5. **Invariant Auditing**:
   * Checks for crossed-book scenarios, `NaN` values, or signals with missing trace provenance.

---

## 4. Instructions for Future Agents: How to Extend

To test a new core module or strategy, you must extend `core_probe`'s diagnostic capabilities. Follow these steps:

### A. Adding a New Core Invariant (Risk or Sanitization checks)
To enforce a new safety rule across all replays (e.g., "Mid-price cannot drop/spike by more than 5% in a single tick"):
1. Open `@/home/yevge/trade-bot-hardcore-spec/core/tool/core_probe/InvariantChecker.hpp` and add a new rule function:
   ```cpp
   void check_extreme_volatility(uint64_t trace_id, const Ticker& tkr, double mid);
   ```
2. Implement the threshold tracking in `@/home/yevge/trade-bot-hardcore-spec/core/tool/core_probe/InvariantChecker.cpp`.
3. If `--invariants strict` is configured, ensure any violation raises `had_violation_ = true` to force exit code `4`.

### B. Adding Slippage & Network Latency Simulation to PaperExecutor
Currently, `PaperExecutor` performs fill matching immediately. To test strategy sensitivity under realistic exchange conditions:
1. Open `@/home/yevge/trade-bot-hardcore-spec/core/src/executor/PaperExecutor.cpp` (or its decorator in the probe).
2. Introduce a configurable delay parameter (e.g., `simulated_latency_ms`) in `CliOptions`.
3. When order events are submitted, buffer them inside a time-priority queue and only process them when simulated virtual clock time matches `submission_time + latency`.

### C. Integrating the RiskManager with Historical Portfolios
Ensure the 15 strict risk rules (daily loss, max positions, leverage limits) are validated on real data:
1. Pass simulated account balances and active positions into `RiskManager` inside `@/home/yevge/trade-bot-hardcore-spec/core/tool/core_probe/ProbePipeline.cpp`.
2. When `PaperExecutor` executes simulated orders, feed those updates directly into `RiskManager::check_order(...)`.
3. Assert that any risk violations are flagged in the final pipeline output log.

---

## 5. Verification Checklist for Commits

Before submitting C++ edits, agents must verify stability using this command sequence:
1. **Rebuild clean**: `./scripts/build.sh debug --quick`
2. **Replay Validation**: Ensure the core successfully replays production data with zero errors:
   `./build/debug/bin/core_probe replay --dump replay/dumps/dump_2026-05-18T12-16-29.ndjson --ticker BTC_USDT --limit 2000`
3. **No Memory Violations**: Run the replays with ASan/UBSan checks enabled and verify exit code `0` (or `4` for expected strict invariant terminations).
