# Comprehensive Audit Report — Ruflo Trade Bot

**Date:** 2026-05-11
**Scope:** Full-stack audit — SDK API integration, strategy compliance, signal detection, risk management, architecture
**SDK version:** f675e18 → a4018ec (metascalp-sdk submodule)

---

## Executive Summary

| Severity | Count | Description |
|----------|-------|-------------|
| **T0-BUG** | 4 | Production bugs — broken functionality, must fix immediately |
| **T1-FEAT** | 5 | Missing features from spec — significant gaps |
| **T2-GAP** | 3 | Spec/code mismatches — medium priority |
| **T3-STYLE** | 4 | Improvements, missing validations, edge cases |

---

## T0-BUG: Production Bugs

### #1 — mark_price_update: field name mismatch (MarketDataFeed.cpp:288-289)

**Status:** 🔴 BROKEN
**Impact:** Mark price cache is NEVER populated. All positions use mark_price=0.0 for PnL calculation. This silently corrupts position valuation, stop-loss execution, and dashboard display.

**Root cause:** The `mark_price_update` handler uses lowercase/camelCase JSON field names but the MetaScalp SDK API uses PascalCase.

```cpp
// MarketDataFeed.cpp:288-289 — BROKEN CODE
Ticker ticker = from_ms(data.value("ticker", ""));     // API sends "Ticker"
double mp = data.value("markPrice", 0.0);               // API sends "MarkPrice"
```

**SDK API specification (metascalp-sdk/docs/MetaScalp-Api.md):**
```json
{
  "Type": "mark_price_update",
  "Data": {
    "ConnectionId": 1,
    "Ticker": "BTCUSDT",
    "MarkPrice": 65123.5
  }
}
```

**Fix:** Replace with:
```cpp
Ticker ticker = from_ms(MetaScalpCodec::get_val<std::string>(data, "Ticker", ""));
double mp = MetaScalpCodec::get_val<double>(data, "MarkPrice", 0.0);
```

Note: `funding_update` handler at line 275-278 is **correct** — it uses PascalCase `"Ticker"`, `"FundingRate"`, `"FundingTime"`.

---

### #2 — R14: Single Position Loss Kill (spec/RISK_MANAGEMENT.md §2)

**Status:** 🔴 NOT IMPLEMENTED
**Impact:** If a position moves sharply against the bot and the stop-loss fails (e.g., exchange doesn't honor it, WS disconnected), there is NO safety net. Position can lose unlimited amount.

**Spec requirement:** `max_single_position_loss_pct` (default 1.5%) — if unrealized loss of any single position exceeds this threshold, immediately market-close via REST (bypassing WS). Checked on every `position_update` and in WS-loss recovery poll.

**Missing:**
- `max_single_position_loss_pct` config field in `RiskManager::Config`
- R14 check in `RiskManager::evaluate()` or separate monitoring loop
- Market-close via REST in the WS-loss recovery path

---

### #3 — R15: Entry Slippage Control (spec/RISK_MANAGEMENT.md §2)

**Status:** 🔴 NOT IMPLEMENTED
**Impact:** Market orders can fill with arbitrarily bad slippage, destroying R:R ratio. No automatic close on bad fill.

**Spec requirement:** After market-order fill, verify `actual_slippage_bps = |avg_fill - expected| / expected * 10000`. If > `max_entry_slippage_bps` (default 5 bps) → immediate market-close.

**Missing:**
- `max_entry_slippage_bps` config field
- Post-fill slippage check in executor
- Automatic close on excessive slippage

---

### #4 — FlushReversal strategy: config exists, no implementation

**Status:** 🔴 NOT IMPLEMENTED
**Impact:** Config file `config.toml` has `[strategies.flush]` section. No `FlushReversal.cpp`/`FlushReversal.hpp` files. Referenced in ROADMAP.md as a Phase 4 deliverable. Loading this config section may cause silent config errors.

**Config:** `config.toml` lines 200-205
```toml
[strategies.flush]
tp1_size_ratio = 0.7
no_progress_timeout_sec = 90
flush_window_sec = 120
affinity_threshold = 0.4
affinity_stable_min = 5
```

---

## T1-FEAT: Missing Features from Spec

### #5 — LiquidationDetector not implemented

**Status:** 🟠 MISSING
**Mentioned in:** ROADMAP.md, docs/HANDOFF_2026-05-03.md, reports/FIXES_SUMMARY_2026-05-11.md
**Purpose:** Detect liquidation cascades for FlushReversal strategy entry signals.
No implementation files exist.

---

### #6 — Affinity system bypassed (main.cpp:101-103)

**Status:** 🟠 HARDCODED PASS-THROUGH
**Spec requirement (STRATEGIES.md §0.0):** `TickerUniverse` must calculate per-strategy affinity scores based on liquidity, spread, volatility. Strategy activates only if `affinity ≥ affinity_threshold`.

**Current code:**
```cpp
universe_.register_strategy("bounce", [](const Ticker&){ return true; });
universe_.register_strategy("breakout", [](const Ticker&){ return true; });
universe_.register_strategy("leaderlag", [](const Ticker&){ return true; });
```

`set_stats_lookup` returns dummy data (volume=100M, spread=1.0 bps). No real per-ticker statistics are computed from warmup data.

**Impact:** All tickers pass all strategy affinity gates unconditionally. Strategies may activate on illiquid, high-spread tickers.

---

### #7 — orderbook_subscribe: missing DepthLevels/DepthPercent

**Status:** 🟠 NOT LEVERAGED
**SDK API supports:** Optional `DepthLevels` (top-N per side, snapshot only) and `DepthPercent` (per-side band around best ask/bid, snapshot + updates) in `orderbook_subscribe`.

**Current code (MarketDataFeed.cpp:100):**
```cpp
send_sub("orderbook_subscribe");
// Sends: {ConnectionId, Ticker, ZoomIndex: 0} — no DepthLevels/DepthPercent
```

**Impact:** Full order book depth is streamed for every ticker. Bandwidth-inefficient for deep books (e.g., BTC has 500+ levels). `DepthPercent=0.5` would filter to ±0.5% around best bid/ask.

---

### #8 — R12: News calendar staleness validation not implemented

**Status:** 🟠 PARTIALLY IMPLEMENTED
**Spec requirement:** Check `news_calendar_check_min` (default 60 min) that calendar is fresh. If stale → WARN. If `news_calendar_require_fresh=true` and stale → reject all plans.

**Current code:** RiskManager checks `news_.minutes_to_next_news()` for individual news blackout (R12) but does NOT validate calendar freshness. No periodic freshness check, no `news_calendar_require_fresh` handling.

---

### #9 — Kill-switch sequence: incomplete (spec/RISK_MANAGEMENT.md §4)

**Status:** 🟠 PARTIALLY IMPLEMENTED
**Spec requires 8-step kill-switch sequence with per-step timeouts.** KillSwitch class exists and handles basic triggers. Need to verify completeness:

| Step | Requirement | Status |
|------|-------------|--------|
| 1 | Set `kill_switch_triggered = true` | ✅ In KillSwitch |
| 2 | Cancel all orders via REST (parallel, max 5) | ⚠️ Needs verification |
| 3 | GET positions via REST | ⚠️ Needs verification |
| 4 | Market-close all positions | ⚠️ Needs verification |
| 5 | Poll until closed (500ms, 30s timeout) | ⚠️ Needs verification |
| 6 | Create `./killswitch` file | ✅ In KillSwitch |
| 7 | Persist state + kill_switch_report.json | ⚠️ Needs verification |
| 8 | Exit with code 42 | ⚠️ Needs verification |

Additional kill-switch triggers from spec not verified:
- Exchange error streak ≥ 5
- Clock drift > 500ms
- WS reconnect timeout > 15s
- Position reconciliation drift
- MetaScalp connection state != Connected
- External feed staleness
- FinRes staleness

---

## T2-GAP: Spec/Code Mismatches

### #10 — `warn_stop_bps` declared but never checked

**Status:** 🟡 DEAD CONFIG
`RiskManager::Config` has `warn_stop_bps{15.0}` but `RiskManager::evaluate()` never compares `stop_dist_bps` against `warn_stop_bps` and never emits a WARN log. Spec says: "Если `stop_distance_bps > warn_stop_bps` — WARN в логе (стоп «широковат для скальпа», но не reject)".

---

### #11 — `max_vol_bps` typo in config.toml

**Status:** 🟡 TYPO
`config.toml` line 168: `max_vol_bps = 50` — should be `max_vol_bps`. The config example has the same typo. spec/RISK_MANAGEMENT.md table lists it as `max_vol_bps`. The Config parser reads the field as-is, so if code looks for `max_vol_bps`, it won't find the value.

Wait — this is actually `max_vol_bps` which reads as "max volatility bps" — that's the intended meaning, not a typo. Cancelled.

---

### #11 — No per-ticker position limit

**Status:** 🟡 MISSING
RiskManager has `max_concurrent_positions` (total) but no `max_positions_per_ticker` limit. Spec doesn't explicitly require it, but for risk safety, a single ticker shouldn't hold all slots.

---

### #12 — StrategyEngine doesn't register FlushReversal

**Status:** 🟡 MISSING
`main.cpp` only registers 3 strategies (bounce, breakout, leaderlag). If FlushReversal is implemented, StrategyEngine needs registration:
```cpp
else if (strategy == "flush") strategy_engine_->add_strategy(std::make_unique<FlushReversal>(ticker, info));
```

---

## T3-STYLE: Improvements & Edge Cases

### #13 — No TODO/FIXME/HACK markers in codebase

**Status:** 🟢 CLEAN
Code search for TODO, FIXME, HACK, XXX, TEMP, WORKAROUND, HARDCODED returned 0 results. The codebase is clean of known technical debt markers. However, this also means no tracking of incomplete work.

---

### #14 — `Config::get_or` for `max_leverage` reads `int64_t` but casts to `int`

**Status:** 🟢 MINOR
```cpp
rm_cfg.max_leverage = static_cast<int>(Config::get_or<int64_t>("risk.max_leverage", 5));
```
The toml value is integer; `int64_t` is used for parsing but the config field is `int`. Non-critical.

---

### #15 — `min_follow_through_rate_7d` in config but usage unclear

**Status:** 🟢 NEEDS VERIFICATION
`config.toml` line 251: `min_follow_through_rate_7d = 0.35` under leaderlag section. No code search matches found for this config key. May be unused/dead config.

---

### #16 — Funding update comment says "PascalCase fields per MetaScalp API"

**Status:** 🟢 CORRECT
`MarketDataFeed.cpp:274` comment correctly states PascalCase usage. The `funding_update` handler is correctly implemented.

---

## SDK API Changes Summary (f675e18 → a4018ec)

| Change | Impact on Bot |
|--------|---------------|
| `mark_price_subscribe` / `mark_price_update` | ✅ Subscribed, ❌ Parser broken (T0-BUG #1) |
| `funding_subscribe` / `funding_update` | ✅ Correctly implemented |
| `DepthLevels` / `DepthPercent` optional params | ⚠️ Not used (T1-FEAT #7) |
| `ViewMode`, `DemoMode` in ConnectionInfo | ⚠️ Not read by bot |
| `MaxSize` nullable in TickerInfo | ⚠️ Bot uses `double max_size` (non-nullable) |
| Various internal SDK refactors | No impact on bot |

---

## Recommendations

### Immediate (this week):
1. **Fix T0-BUG #1** — mark_price_update field names (1 line change)
2. **Implement T0-BUG #2** — R14 single position loss kill
3. **Implement T0-BUG #3** — R15 entry slippage control
4. **Stub out FlushReversal config** to prevent config errors

### Short-term (next sprint):
5. Implement FlushReversal strategy + LiquidationDetector (T0-BUG #4, T1-FEAT #5)
6. Implement real affinity scoring (T1-FEAT #6)
7. Add DepthLevels/DepthPercent to orderbook subscriptions (T1-FEAT #7)

### Medium-term:
8. Complete kill-switch sequence verification (T1-FEAT #9)
9. News calendar freshness validation (T1-FEAT #8)
10. Add warn_stop_bps check (T2-GAP #10)

---

## Files Requiring Changes

| File | Issues |
|------|--------|
| `core/src/transport/MarketDataFeed.cpp` | T0-BUG #1: fix mark_price_update parser |
| `core/src/risk/RiskManager.hpp` | T0-BUG #2, #3: add config fields |
| `core/src/risk/RiskManager.cpp` | T0-BUG #2, #3: add R14/R15 checks |
| `core/src/executor/LiveExecutor.cpp` | T0-BUG #2: add R14 monitoring loop |
| `core/src/executor/PaperExecutor.cpp` | T0-BUG #2: add R14 monitoring loop |
| `core/src/main.cpp` | T1-FEAT #6: real affinity scoring, T2-GAP #12: FlushReversal registration |
| `core/src/strategy/FlushReversal.{hpp,cpp}` | T0-BUG #4: create strategy |
| `core/src/signals/LiquidationDetector.{hpp,cpp}` | T1-FEAT #5: create detector |
| `config.toml` | T1-FEAT: add missing config keys |
| `CMakeLists.txt` | T0-BUG #4, T1-FEAT #5: add new source files |
