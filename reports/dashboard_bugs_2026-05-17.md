# Dashboard Bug Analysis — 2026-05-17

## Executive Summary

Обнаружено **12 багов** в React дашборде:
- 🔴 **3 CRITICAL** — потеря данных, race conditions
- 🟠 **4 HIGH** — неточные метрики, stale визуализация
- 🟡 **4 MEDIUM** — производительность, UX
- 🟢 **1 LOW** — косметика

## Critical Bugs

### D1-CRITICAL: Chart History Race Condition
**Файл**: `dashboard/src/store/useTradeStore.ts:165-175`  
**Проблема**: При быстром потоке данных (100ms тики) `existingTs.has()` может пропустить дубликаты из-за race condition между проверкой и добавлением.

```typescript
// BUGGY CODE
const existingTs = new Set(state.chartHistory.map(p => p.ts_unix_ms));
const newPoints = (data.chart_history as ChartPoint[]).filter(
  p => p.ts_unix_ms > 0 && !existingTs.has(p.ts_unix_ms)
);
if (newPoints.length > 0) {
  mergedChartHistory = [...state.chartHistory, ...newPoints]
    .sort((a, b) => a.ts_unix_ms - b.ts_unix_ms)
    .slice(-MAX_CHART_POINTS);
}
```

**Impact**: Дублирующие точки → неправильная сортировка → скачки цены на графике.

**Fix**:
```typescript
// Build map for O(1) dedup during merge
const tsMap = new Map(state.chartHistory.map(p => [p.ts_unix_ms, p]));
for (const p of (data.chart_history as ChartPoint[])) {
  if (p.ts_unix_ms > 0 && !tsMap.has(p.ts_unix_ms)) {
    tsMap.set(p.ts_unix_ms, p);
  }
}
mergedChartHistory = Array.from(tsMap.values())
  .sort((a, b) => a.ts_unix_ms - b.ts_unix_ms)
  .slice(-MAX_CHART_POINTS);
```

---

### D2-CRITICAL: Dirty Flags Not Reset on Ticker Switch
**Файл**: `dashboard/src/components/AdvancedChart.tsx:1650-1660`  
**Проблема**: При смене тикера `dirtyRef.current` не сбрасывается → старые данные рендерятся на новом графике.

```typescript
// MISSING RESET
useEffect(() => {
  yEmaRef.current.cur = null;
  resolvedRangeRef.current = null;
  setPriceMeta(null);
  // dirtyRef.current = { density: true, price: true, ... }; // MISSING!
}, [ticker]);
```

**Impact**: Density/walls от предыдущего тикера видны 1-2 секунды на новом графике.

**Fix**:
```typescript
useEffect(() => {
  yEmaRef.current.cur = null;
  resolvedRangeRef.current = null;
  setPriceMeta(null);
  dirtyRef.current = { density: true, price: true, position: true, ribbon: true, signals: true };
}, [ticker]);
```

---

### D3-CRITICAL: Ticker Switch Leaves Stale Iceberg/Density
**Файл**: `dashboard/src/store/useTradeStore.ts:290-295`  
**Проблема**: При смене тикера очищается только `orderbook`, но `densityHistory` и `icebergEvents` остаются от старого тикера.

```typescript
// INCOMPLETE CLEAR
const tickerChanged = !!data.selected_ticker && data.selected_ticker !== state.activeTicker;
return {
  orderbook: {
    bids: data.bids_top20 ?? (tickerChanged ? [] : state.orderbook.bids),
    // ...
  },
  // densityHistory: tickerChanged ? [] : ..., // MISSING!
  // icebergEvents: tickerChanged ? [] : ...,  // MISSING!
```

**Impact**: Iceberg pulses и density columns от BTC видны на ETH графике.

**Fix**:
```typescript
densityHistory: tickerChanged ? [] : (Array.isArray(data.density_history) ? data.density_history : state.densityHistory),
icebergEvents: tickerChanged ? [] : (data.iceberg_events ?? state.icebergEvents),
```

---

## High Priority Bugs

### D4-HIGH: Price Pulse on Stale Data
**Файл**: `dashboard/src/components/AdvancedChart.tsx:1680-1685`  
**Проблема**: Pulse animation продолжается до 3000ms после последней точки → создаёт иллюзию "live" на мёртвых данных.

```typescript
// TOO GENEROUS
if (freshTs && Date.now() - freshTs < 3000) dirtyRef.current.price = true;
```

**Impact**: Оператор думает, что данные свежие, но feed уже 2.5s stale.

**Fix**: Уменьшить порог до 1000ms:
```typescript
if (freshTs && Date.now() - freshTs < 1000) dirtyRef.current.price = true;
```

---

### D5-HIGH: Signal Aggregation Window Too Wide
**Файл**: `dashboard/src/store/useTradeStore.ts:107-115`  
**Проблема**: 1000ms окно агрегации → быстрые сигналы (100-200ms apart) схлопываются в один.

```typescript
// TOO WIDE
const existing = state.signals.find(s => 
  s.ticker === signal.ticker && 
  s.type === signal.type && 
  (signal.timestamp - s.timestamp < 1000) // 1s window
);
```

**Impact**: Пропускаются rapid-fire сигналы (например, 3 DensityDetected за 500ms).

**Fix**: Уменьшить до 300ms:
```typescript
(signal.timestamp - s.timestamp < 300)
```

---

### D6-HIGH: Regime Hysteresis Can Stick
**Файл**: `dashboard/src/components/AdvancedChart.tsx:330-345`  
**Проблема**: Hysteresis logic может "залипнуть" на старом режиме, если метрики колеблются вокруг порога.

```typescript
// STICKY LOGIC
const stillPrev = regimeFor(vol, imb, agg, VOL_HI * (1 - H), IMB_HI * (1 - H), AGG_HI * (1 - H));
if (stillPrev === prev) regime = prev;
```

**Impact**: Pill показывает MOMENTUM, когда рынок уже NEUTRAL.

**Fix**: Добавить timeout — если режим не меняется 5s, force re-eval:
```typescript
const REGIME_TIMEOUT_MS = 5000;
const lastRegimeChangeRef = useRef(Date.now());
if (raw !== prev) {
  const stillPrev = regimeFor(vol, imb, agg, VOL_HI * (1 - H), IMB_HI * (1 - H), AGG_HI * (1 - H));
  if (stillPrev === prev && Date.now() - lastRegimeChangeRef.current < REGIME_TIMEOUT_MS) {
    regime = prev;
  } else {
    regime = raw;
    lastRegimeChangeRef.current = Date.now();
  }
}
```

---

### D7-HIGH: Journal hold_ms Unit Ambiguity
**Файл**: `dashboard/src/store/useTradeStore.ts:220-225`  
**Проблема**: Код предполагает `hold_ms` в milliseconds, но сервер может отправлять seconds → `entryTsMs` оказывается в будущем.

```typescript
// UNIT ASSUMPTION
const entryTsMs = j.entry_ts_unix_ms ?? j.plan?.entry_ts ?? (tsMs - (j.hold_ms ?? 0));
if ((import.meta as any).env?.DEV && j.exit_price && entryTsMs >= tsMs) {
  console.assert(false, `[journal] entryTsMs (${entryTsMs}) >= exitTsMs (${tsMs}) — hold_ms unit suspect`, j);
}
```

**Impact**: Journal markers на графике схлопываются в одну точку (entry == exit).

**Fix**: Добавить heuristic detection:
```typescript
let holdMs = j.hold_ms ?? 0;
// If hold_ms > 10000 (10s) but trade duration looks short, assume seconds
if (holdMs > 10000 && j.pnl_pct && Math.abs(j.pnl_pct) < 0.5) {
  holdMs *= 1000; // convert to ms
}
const entryTsMs = j.entry_ts_unix_ms ?? j.plan?.entry_ts ?? (tsMs - holdMs);
```

---

## Medium Priority Bugs

### D8-MEDIUM: Crosshair Info Recomputed Every Frame
**Файл**: `dashboard/src/components/AdvancedChart.tsx:2010-2040`  
**Проблема**: `useMemo` пересчитывает deltas каждый кадр, даже если мышь не двигалась.

```typescript
// EXPENSIVE MEMO
const crosshairInfo = useMemo(() => {
  if (!crosshair || !mainChartRef.current || !chartHistory.length) return null;
  // ... binary search + delta calc ...
}, [crosshair, chartHistory, activeTrade]); // chartHistory changes every 100ms!
```

**Impact**: 60fps → 30fps при движении мыши на слабых машинах.

**Fix**: Memoize по `crosshair.x/y` + `activeTrade.id`:
```typescript
const crosshairInfo = useMemo(() => {
  // ...
}, [crosshair?.x, crosshair?.y, chartHistory.length, activeTrade?.id]);
```

---

### D9-MEDIUM: Price CustomEvent Throttled Too Much
**Файл**: `dashboard/src/components/AdvancedChart.tsx:1730-1745`  
**Проблема**: Dispatch только при изменении цены + 200ms throttle для метрик → быстрые колебания (±0.01% за 100ms) не видны в header.

```typescript
// THROTTLED
if (lp > 0 && lp !== lastDispatchedPriceRef.current) {
  lastDispatchedPriceRef.current = lp;
  window.dispatchEvent(new CustomEvent<PriceUpdateDetail>(`chart-update-${currentTicker}`, { ... }));
}
// ...
if (now - lastMetaPush > 200) { // 200ms throttle
  lastMetaPush = now;
  const m = computeMeta(chartHistory);
  if (m) setPriceMeta(m);
}
```

**Impact**: Header показывает "LIVE 0.5s", но цена не обновляется 200ms.

**Fix**: Убрать throttle для price, оставить только для regime:
```typescript
// Dispatch price immediately
if (lp > 0 && lp !== lastDispatchedPriceRef.current) {
  lastDispatchedPriceRef.current = lp;
  window.dispatchEvent(...);
  const m = computeMeta(chartHistory);
  if (m) setPriceMeta(m);
}
// Regime only every 500ms
if (now - lastMetaPush > 500) {
  lastMetaPush = now;
  const newRegime = classifyRegime(chartHistory, lastRegimeRef.current);
  if (newRegime.regime !== lastRegimeRef.current) {
    lastRegimeRef.current = newRegime.regime;
    setRegimeMeta(newRegime);
  }
}
```

---

### D10-MEDIUM: Ribbon P95 Cache Invalidation
**Файл**: `dashboard/src/components/AdvancedChart.tsx:240-255`  
**Проблема**: Cache key `{ ref, start, len }` не учитывает изменения внутри массива → stale P95 при обновлении точек.

```typescript
// SHALLOW CACHE
if (_ribbonP95Cache && _ribbonP95Cache.ref === pts
    && _ribbonP95Cache.start === startIdx && _ribbonP95Cache.len === len) {
  return _ribbonP95Cache.val;
}
```

**Impact**: Ribbon scale "залипает" на старом max, новые всплески обрезаются.

**Fix**: Добавить hash последних N точек:
```typescript
const lastHash = pts.slice(-10).reduce((h, p) => h + p.buy_vol_5s + p.sell_vol_5s, 0);
if (_ribbonP95Cache && _ribbonP95Cache.ref === pts && _ribbonP95Cache.start === startIdx 
    && _ribbonP95Cache.len === len && _ribbonP95Cache.lastHash === lastHash) {
  return _ribbonP95Cache.val;
}
// ...
_ribbonP95Cache = { ref: pts, start: startIdx, len, val, lastHash };
```

---

### D11-MEDIUM: Wall Threshold Cache Stale
**Файл**: `dashboard/src/components/AdvancedChart.tsx:220-230`  
**Проблема**: Аналогично D10 — cache по reference, не по содержимому.

```typescript
// SHALLOW CACHE
if (_wallThreshCache?.ref === levels) return _wallThreshCache.val;
```

**Impact**: Walls не обновляются при изменении размеров уровней.

**Fix**: Добавить hash top-5 sizes:
```typescript
const topHash = levels.slice(0, 5).reduce((h, l) => h + l.size, 0);
if (_wallThreshCache?.ref === levels && _wallThreshCache.topHash === topHash) {
  return _wallThreshCache.val;
}
// ...
_wallThreshCache = { ref: levels, val, topHash };
```

---

## Low Priority Bugs

### D12-LOW: Extra Position Count Badge Overlap
**Файл**: `dashboard/src/components/AdvancedChart.tsx:1990-1995`  
**Проблема**: Badge "+2 more" может перекрывать PnL pill при узком экране.

**Impact**: Косметика, не влияет на функциональность.

**Fix**: Добавить `flex-wrap` в родительский контейнер.

---

## Recommended Fixes Priority

### Immediate (сегодня)
1. **D1** — Chart history race condition (потеря данных)
2. **D2** — Dirty flags reset (визуальный баг)
3. **D3** — Ticker switch stale data (confusion)

### This Week
4. **D4** — Pulse on stale data (false "live" indicator)
5. **D5** — Signal aggregation (пропуск сигналов)
6. **D9** — Price throttle (header lag)

### Next Sprint
7. **D6** — Regime hysteresis (UX)
8. **D7** — Journal hold_ms (data quality)
9. **D8** — Crosshair perf (60fps)
10. **D10-D11** — Cache invalidation (accuracy)

### Backlog
11. **D12** — Badge overlap (polish)

---

## Testing Checklist

- [ ] D1: Stress test с 10Hz chart updates, проверить дубликаты
- [ ] D2: Переключить тикер 5 раз подряд, проверить density flash
- [ ] D3: Переключить BTC→ETH, проверить iceberg pulses
- [ ] D4: Отключить feed, проверить pulse stop через 1s
- [ ] D5: Генерировать 5 сигналов за 500ms, проверить count
- [ ] D6: Колебания vol вокруг 8bps, проверить regime flip
- [ ] D7: Mock journal с hold_ms=30 (seconds), проверить entry position
- [ ] D8: Профилировать crosshair move, проверить 60fps
- [ ] D9: Быстрые колебания ±0.01%, проверить header update rate
- [ ] D10-D11: Изменить top bid size, проверить wall/ribbon update

---

**Last Updated**: 2026-05-17  
**Severity Distribution**: 3 CRITICAL, 4 HIGH, 4 MEDIUM, 1 LOW  
**Estimated Fix Time**: 8-12 hours
