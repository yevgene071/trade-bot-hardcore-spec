import React, { useEffect, useRef, useCallback, useState, useMemo } from 'react';
import { useTradeStore } from '../store/useTradeStore';
import { marketDataService } from '../services/MarketDataService';
import type { ChartPoint, ObLevel, IcebergEvent, DensityColumn, Trade, JournalEntry } from '../types';
import { cn } from '../lib/utils';

// ─────────────────────────────────────────────────────────────────────────────
// Дизайн-философия:
//   L1 (Action)  — позиция, SL, TP, маркеры сделок, current price.  Полная яркость.
//   L2 (Price)   — линия цены, spread shadow, ценовой тег.           Средняя яркость.
//   L3 (Context) — density, walls, ghost, ribbon, grid.              Приглушено.
// ─────────────────────────────────────────────────────────────────────────────

interface ChartColors {
  densityRgb:  { cold: readonly [number,number,number]; hot: readonly [number,number,number] };
  wallBidRgb:  readonly [number,number,number];
  wallAskRgb:  readonly [number,number,number];
  priceLine:   string;
  priceAgg:    string;
  spreadShadow:string;
  ghostHigh:   string;
  ghostMid:    string;
  ghostLow:    string;
  buyRibbon:   readonly [number,number,number];
  sellRibbon:  readonly [number,number,number];
  sigBuy:      string;
  sigSell:     string;
  icebergRgb:  readonly [number,number,number];
  // L1 — Position layer
  entryLongRgb:  readonly [number,number,number];
  entryShortRgb: readonly [number,number,number];
  slRgb:         readonly [number,number,number];
  tpRgb:         readonly [number,number,number];
  causeTpRgb:    readonly [number,number,number];
  causeSlRgb:    readonly [number,number,number];
  causeSignalRgb:readonly [number,number,number];
  causeTimeRgb:  readonly [number,number,number];
  // Regime tints
  regimeMomentum: string;
  regimeFlush:    string;
  regimeAccum:    string;
  regimeDistrib:  string;
  regimeNeutral:  string;
}

let _chartColorsCache: ChartColors | null = null;
let _chartColorsThemeKey = '';

function getChartColors(): ChartColors {
  const themeKey = document.documentElement.getAttribute('data-theme') || document.documentElement.className || 'dark';
  if (_chartColorsCache && _chartColorsThemeKey === themeKey) return _chartColorsCache;
  _chartColorsThemeKey = themeKey;
  const s = getComputedStyle(document.documentElement);
  const rgb = (v: string, fb: readonly [number,number,number] = [255,255,255]) => {
    const raw = s.getPropertyValue(v).trim();
    if (!raw) return fb;
    const parts = raw.split(/\s+/).map(Number);
    return [parts[0] ?? fb[0], parts[1] ?? fb[1], parts[2] ?? fb[2]] as const;
  };
  const hex = (v: string, fb = '#ffffff') => {
    const parts = rgb(v);
    if (parts[0] === 255 && parts[1] === 255 && parts[2] === 255 && fb === '#ffffff') return fb;
    return '#' + parts.map(c => c.toString(16).padStart(2,'0')).join('');
  };
  const rgba = (v: string, a: number) => `rgba(${rgb(v).join(',')},${a})`;
  _chartColorsCache = {
    densityRgb:  { cold: rgb('--chart-cold', [40,55,70]), hot: rgb('--chart-hot', [192,136,40]) },
    wallBidRgb:  rgb('--chart-green', [38,194,110]),
    wallAskRgb:  rgb('--chart-red',   [224,80,80]),
    priceLine:   hex('--chart-price', '#d8e8f8'),
    priceAgg:    hex('--chart-amber', '#c08828'),
    spreadShadow:rgba('--chart-spread', 0.06),
    ghostHigh:   rgba('--chart-ghost-hi', 0.55),
    ghostMid:    rgba('--chart-ghost-md', 0.30),
    ghostLow:    rgba('--chart-ghost-lo', 0.12),
    buyRibbon:   rgb('--chart-green', [38,194,110]),
    sellRibbon:  rgb('--chart-red',   [224,80,80]),
    sigBuy:      rgba('--chart-green', 0.55),
    sigSell:     rgba('--chart-red',   0.55),
    icebergRgb:  rgb('--chart-amber', [192,136,40]),
    // L1 — Position
    entryLongRgb:   rgb('--chart-entry-long',  [60,220,140]),
    entryShortRgb:  rgb('--chart-entry-short', [240,90,90]),
    slRgb:          rgb('--chart-sl',          [240,90,90]),
    tpRgb:          rgb('--chart-tp',          [60,220,140]),
    causeTpRgb:     rgb('--chart-cause-tp',     [60,220,140]),
    causeSlRgb:     rgb('--chart-cause-sl',     [240,90,90]),
    causeSignalRgb: rgb('--chart-cause-signal', [90,150,240]),
    causeTimeRgb:   rgb('--chart-cause-time',   [140,150,160]),
    regimeMomentum: hex('--chart-regime-momentum','#3cdc8c'),
    regimeFlush:    hex('--chart-regime-flush',   '#f05a5a'),
    regimeAccum:    hex('--chart-regime-accum',   '#c08828'),
    regimeDistrib:  hex('--chart-regime-distrib', '#a070d0'),
    regimeNeutral:  hex('--chart-regime-neutral', '#7a8fa8'),
  };
  return _chartColorsCache;
}

const TICK_MS     = 100;
const CORR_SHOW   = 0.5;   // было 0.3 — приглушаем ghost
const CORR_HIGH   = 0.8;
const PRICE_PAD   = 0.01;

// ── Helpers ───────────────────────────────────────────────────────────────────

function findWindowStart(pts: { ts_unix_ms: number }[], minTs: number): number {
  let lo = 0, hi = pts.length;
  while (lo < hi) {
    const mid = (lo + hi) >>> 1;
    if (pts[mid].ts_unix_ms < minTs) lo = mid + 1;
    else hi = mid;
  }
  return lo;
}

function priceRange(pts: ChartPoint[], extras: number[] = []) {
  let mn = Infinity, mx = -Infinity;
  for (const p of pts) {
    if (p.mid <= 0) continue;
    if (p.mid < mn) mn = p.mid;
    if (p.mid > mx) mx = p.mid;
  }
  // Включаем SL/TP/entry в Y-диапазон, чтобы они всегда были видны
  for (const v of extras) {
    if (!isFinite(v) || v <= 0) continue;
    if (v < mn) mn = v;
    if (v > mx) mx = v;
  }
  if (!isFinite(mn)) return { min: 0, max: 1 };

  const minSpread = mn * 0.001;
  let pad = (mx - mn) * PRICE_PAD;
  if ((mx - mn) < minSpread) {
    const center = (mx + mn) / 2;
    mn = center - minSpread / 2;
    mx = center + minSpread / 2;
    pad = 0;
  }
  return { min: mn - pad, max: mx + pad };
}

type Range = { min: number; max: number };
function smoothRange(target: Range, ema: { cur: Range | null }): Range {
  const prev = ema.cur;
  if (!prev) { ema.cur = { ...target }; return target; }
  const span = prev.max - prev.min || 1;
  const jumped =
    Math.abs(target.min - prev.min) / span > 0.35 ||
    Math.abs(target.max - prev.max) / span > 0.35;
  if (jumped) { ema.cur = { ...target }; return target; }
  const a = 0.12;
  const next = {
    min: prev.min + a * (target.min - prev.min),
    max: prev.max + a * (target.max - prev.max),
  };
  ema.cur = next;
  return next;
}

const TIME_WINDOW_MS = 600000;
const MIN_WINDOW_MS  = 5000; // wheel can zoom down to 5s

// ARCH-01: xWindow always requires explicit windowMs/rightTs from the caller's
// viewRef. No module globals — each AdvancedChart instance threads its own
// view state so multiple mounts (StrictMode remount, ErrorBoundary recovery)
// never conflict.
function xWindow(pts: ChartPoint[], windowMsOverride = 0, rightTsOverride = 0) {
  const now = Date.now();
  let lastTs = 0, firstTs = 0, found = false;
  for (let i = pts.length - 1; i >= 0; i--) {
    if (pts[i].mid > 0) { lastTs = pts[i].ts_unix_ms; found = true; break; }
  }
  for (let i = 0; i < pts.length; i++) {
    if (pts[i].mid > 0) { firstTs = pts[i].ts_unix_ms; break; }
  }
  const isFresh = found && (now - lastTs < 30000);
  const anchorTs = isFresh ? now : (lastTs || now);
  const span = found ? (anchorTs - firstTs) : TIME_WINDOW_MS;
  const base = Math.max(MIN_WINDOW_MS, Math.min(TIME_WINDOW_MS, span * 1.08));
  const windowMs = windowMsOverride > 0
    ? Math.max(MIN_WINDOW_MS, Math.min(TIME_WINDOW_MS, windowMsOverride))
    : base;
  const rightTs = rightTsOverride > 0 ? rightTsOverride : anchorTs;
  return { firstTs: rightTs - windowMs, lastTs: rightTs, windowMs };
}

const makeXOf = (lastTs: number, windowMs: number, W: number) =>
  (ts: number) => Math.max(0, Math.min(W, (1 - (lastTs - ts) / windowMs) * W));

const toY = (price: number, H: number, mn: number, mx: number) =>
  H - ((price - mn) / (mx - mn)) * H;

function sanePriceBounds(pts: ChartPoint[]) {
  const mids = pts.filter(p => p.mid > 0).map(p => p.mid).sort((a, b) => a - b);
  if (!mids.length) return null;
  const median = mids[Math.floor(mids.length / 2)];
  const lo = median * 0.2, hi = median * 5;
  let first: ChartPoint | null = null, last: ChartPoint | null = null;
  for (const p of pts) {
    if (p.mid > lo && p.mid < hi) { if (!first) first = p; last = p; }
  }
  return first && last ? { first, last } : null;
}

// D11-FIX: Add hash of top-5 sizes to detect content changes
let _wallThreshCache: { ref: unknown; val: number; topHash: number; ticker: string } | null = null;
function wallThreshold(levels: ObLevel[], ticker: string) {
  const topHash = levels.slice(0, 5).reduce((h, l) => h + l.size, 0);
  if (_wallThreshCache?.ref === levels && _wallThreshCache.topHash === topHash && _wallThreshCache.ticker === ticker) return _wallThreshCache.val;
  if (!levels.length) { _wallThreshCache = { ref: levels, val: Infinity, topHash: 0, ticker }; return Infinity; }
  const sorted = [...levels].map(l => l.size).sort((a, b) => a - b);
  const val = sorted[Math.floor(sorted.length * 0.8)] ?? Infinity;
  _wallThreshCache = { ref: levels, val, topHash, ticker };
  return val;
}

// D10-FIX: Add hash of last 10 points to detect content changes
let _ribbonP95Cache: { ref: unknown; start: number; len: number; val: number; lastHash: number; ticker: string } | null = null;
function ribbonPerc95(pts: ChartPoint[], startIdx: number, ticker: string): number {
  const len = pts.length;
  const lastHash = pts.slice(-10).reduce((h, p) => h + p.buy_vol_5s + p.sell_vol_5s, 0);
  if (_ribbonP95Cache && _ribbonP95Cache.ref === pts
      && _ribbonP95Cache.start === startIdx && _ribbonP95Cache.len === len
      && _ribbonP95Cache.lastHash === lastHash && _ribbonP95Cache.ticker === ticker) {
    return _ribbonP95Cache.val;
  }
  const n = len - startIdx;
  if (n <= 0) { _ribbonP95Cache = { ref: pts, start: startIdx, len, val: 1, lastHash, ticker }; return 1; }
  const scratch = new Float64Array(n);
  for (let i = 0; i < n; i++) {
    const p = pts[startIdx + i];
    scratch[i] = Math.max(p.buy_vol_5s, p.sell_vol_5s);
  }
  scratch.sort();
  const val = scratch[Math.floor(n * 0.95)] || 1;
  _ribbonP95Cache = { ref: pts, start: startIdx, len, val, lastHash, ticker };
  return val;
}

function getDpr() { return window.devicePixelRatio || 1; }

function findClosestByTs(pts: ChartPoint[], targetTs: number, lo: number, hi: number): number {
  // FUNC-08: guard empty / out-of-range bounds before indexing pts[lo|hi]
  if (lo < 0 || lo >= pts.length || hi >= pts.length || lo > hi) return Math.max(0, Math.min(lo, pts.length - 1));
  if (targetTs <= pts[lo].ts_unix_ms) return lo;
  if (targetTs >= pts[hi].ts_unix_ms) return hi;
  let l = lo, r = hi;
  while (l < r - 1) {
    const mid = (l + r) >>> 1;
    if (pts[mid].ts_unix_ms <= targetTs) l = mid; else r = mid;
  }
  return (targetTs - pts[l].ts_unix_ms < pts[r].ts_unix_ms - targetTs) ? l : r;
}

// PERF-02: replace [...arr].reverse().find() — no allocation
function findLastValid(pts: ChartPoint[]): ChartPoint | undefined {
  for (let i = pts.length - 1; i >= 0; i--) {
    if (pts[i].mid > 0) return pts[i];
  }
  return undefined;
}

// Significant-figure pricing: a flat toFixed(4) renders 0.00004 → "0.0000",
// so a +50% move to 0.00006 looks identical. Scale decimals to the magnitude
// so micro-cap tokens always show ~5 significant digits.
// TYPE-02: side may arrive as LONG/SHORT or BUY/SELL depending on source.
// One typed normalizer replaces scattered `(x.side as string) === 'BUY'` casts.
const sideIsLong = (side?: string) => {
  const s = (side ?? 'LONG').toUpperCase();
  return s === 'LONG' || s === 'BUY';
};

const fmtPrice = (p: number) => {
  if (!isFinite(p) || p === 0) return '0';
  const abs = Math.abs(p);
  if (abs >= 1000) return p.toLocaleString('en-US', { maximumFractionDigits: 0 }); // DISP-01: thousands separator
  if (abs >= 1)    return p.toFixed(2);
  const decimals = Math.min(12, Math.max(4, 4 - Math.floor(Math.log10(abs))));
  return p.toFixed(decimals);
};

// Decimals needed to distinguish a given price step (for tick-aligned axes).
const decimalsFor = (step: number) => {
  if (!isFinite(step) || step <= 0) return 4;
  return Math.min(12, Math.max(0, Math.ceil(-Math.log10(step)) + 1));
};

// Tick values on a 1/2/5 × 10^k ladder within [min,max] — "round" price points.
function niceTicks(min: number, max: number, target = 5): number[] {
  const span = max - min;
  if (!isFinite(span) || span <= 0) return [min];
  const rawStep = span / target;
  const mag = Math.pow(10, Math.floor(Math.log10(rawStep)));
  const norm = rawStep / mag;
  const step = (norm < 1.5 ? 1 : norm < 3 ? 2 : norm < 7 ? 5 : 10) * mag;
  const out: number[] = [];
  for (let v = Math.ceil(min / step) * step; v <= max + step * 1e-6; v += step) out.push(v);
  return out.length ? out : [min];
}

// DISP-04: round wall-clock step (1s…6h) for the time axis so labels snap to
// fixed instants and don't drift with the right edge while panning.
const TIME_STEPS_MS = [1e3, 2e3, 5e3, 1e4, 15e3, 3e4, 6e4, 12e4, 3e5, 6e5, 9e5, 18e5, 36e5, 72e5];
function niceTimeStepMs(windowMs: number, target = 5): number {
  for (const s of TIME_STEPS_MS) if (windowMs / s <= target) return s;
  return TIME_STEPS_MS[TIME_STEPS_MS.length - 1];
}

// ── Market regime classification ──────────────────────────────────────────────
// Деривируется из последних N точек: imbalance, volatility, tape_aggression.

type Regime = 'MOMENTUM' | 'FLUSH' | 'ACCUMULATION' | 'DISTRIBUTION' | 'NEUTRAL';

function regimeFor(vol: number, imb: number, agg: number, VOL: number, IMB: number, AGG: number): Regime {
  if (vol > VOL && (agg > AGG || imb > IMB)) return 'MOMENTUM';
  if (vol > VOL && (agg < -AGG || imb < -IMB)) return 'FLUSH';
  if (vol <= VOL && imb > IMB) return 'ACCUMULATION';
  if (vol <= VOL && imb < -IMB) return 'DISTRIBUTION';
  return 'NEUTRAL';
}

// D6-FIX: Add timestamp tracking to prevent regime from sticking indefinitely
let _lastRegimeChange = 0;
const REGIME_TIMEOUT_MS = 5000;

function classifyRegime(pts: ChartPoint[], prev: Regime = 'NEUTRAL'): { regime: Regime; vol: number; imb: number; agg: number } {
  if (!pts.length) return { regime: 'NEUTRAL', vol: 0, imb: 0, agg: 0 };
  const tail = pts.slice(Math.max(0, pts.length - 30));
  let vol = 0, imb = 0, agg = 0, n = 0;
  for (const p of tail) {
    if (p.mid <= 0) continue;
    vol += p.volatility_1min_bps ?? 0;
    imb += p.imbalance ?? 0;
    agg += p.tape_aggression ?? 0;
    n++;
  }
  if (!n) return { regime: 'NEUTRAL', vol: 0, imb: 0, agg: 0 };
  vol /= n; imb /= n; agg /= n;

  const VOL_HI = 8;
  const IMB_HI = 0.25;
  const AGG_HI = 0.20;
  const H = 0.20;

  const raw = regimeFor(vol, imb, agg, VOL_HI, IMB_HI, AGG_HI);
  let regime = raw;
  const now = Date.now();
  
  if (raw !== prev) {
    const stillPrev = regimeFor(vol, imb, agg, VOL_HI * (1 - H), IMB_HI * (1 - H), AGG_HI * (1 - H));
    // Force switch if stuck for > 5s, otherwise use hysteresis
    if (stillPrev === prev && now - _lastRegimeChange < REGIME_TIMEOUT_MS) {
      regime = prev;
    } else {
      regime = raw;
      _lastRegimeChange = now;
    }
  } else if (raw === prev) {
    _lastRegimeChange = now;
  }

  return { regime, vol, imb, agg };
}

function regimeColor(regime: Regime, C: ChartColors): string {
  switch (regime) {
    case 'MOMENTUM':     return C.regimeMomentum;
    case 'FLUSH':        return C.regimeFlush;
    case 'ACCUMULATION': return C.regimeAccum;
    case 'DISTRIBUTION': return C.regimeDistrib;
    default:             return C.regimeNeutral;
  }
}

// ── Draw: Density Field (МУТЕД: альфа ↓, без красного) ───────────────────────
function drawDensity(
  canvas: HTMLCanvasElement,
  bids: ObLevel[], asks: ObLevel[],
  dh: DensityColumn[],
  lastTs: number, windowMs: number,
  mn: number, mx: number,
  ticker: string,
) {
  const ctx = canvas.getContext('2d');
  if (!ctx) return;
  const C = getChartColors();
  const dpr  = getDpr();
  const W    = canvas.width  / dpr;
  const H    = canvas.height / dpr;
  if (W <= 0 || H <= 0) return; // FEAT-04: skip render on zero-size canvas
  const physW = canvas.width;
  const physH = canvas.height;
  // Combined OB levels — used by fallback heatmap, OB column and wall threshold.
  // drawDensity is dirty-gated (PERF-01), so this runs at OB rate (~2–5 Hz), not 60 fps.
  const levels = bids.concat(asks);

  ctx.save();
  ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
  ctx.clearRect(0, 0, W, H);
  const [cr, cg, cb] = C.densityRgb.cold;
  const [hr, hg, hb] = C.densityRgb.hot;

  const MAX_ALPHA = 0.30; // было 0.45 — приглушаем фон

  if (dh && dh.length) {
    const minVisibleTs = lastTs - windowMs;
    const startIdx = findWindowStart(dh, minVisibleTs);
    const visibleDh = dh.slice(startIdx);
    const rx = makeXOf(lastTs, windowMs, W);
    // MATH-10: derive the column interval from the data instead of a hardcoded
    // 500ms — the server's density cadence is not fixed.
    const colInterval = visibleDh.length > 1
      ? Math.max(1, visibleDh[1].ts_unix_ms - visibleDh[0].ts_unix_ms)
      : 500;

    for (let k = 0; k < visibleDh.length; k++) {
      const col = visibleDh[k];
      const xEnd = k === visibleDh.length - 1 ? W : rx(col.ts_unix_ms);
      const xStart = k === 0 ? Math.max(0, rx(col.ts_unix_ms - colInterval)) : rx(visibleDh[k-1].ts_unix_ms);
      const w = Math.max(0.5, xEnd - xStart);
      if (xStart + w < 0 || xStart > W || col.hi <= col.lo) continue;

      const yLo = toY(col.hi, H, mn, mx);
      const yHi = toY(col.lo, H, mn, mx);
      const rectY = Math.min(yLo, yHi);
      const rectH = Math.abs(yHi - yLo);

      // MATH-05: a single-bin column has no gradient axis (step = 1/0). Render
      // it as a flat block instead of feeding NaN stops to addColorStop().
      if (col.bins.length < 2) {
        const t = Math.pow(col.bins[0] / 255, 0.85);
        if (t > 0.02) {
          ctx.fillStyle = `rgba(${cr + (hr-cr)*t},${cg + (hg-cg)*t},${cb + (hb-cb)*t},${t * MAX_ALPHA})`;
          ctx.fillRect(xStart, rectY, w, rectH);
        }
        continue;
      }

      const grad = ctx.createLinearGradient(0, yHi, 0, yLo);
      const step = 1 / (col.bins.length - 1);
      let hasVisibleBins = false;
      for (let i = 0; i < col.bins.length; i++) {
        const t = Math.pow(col.bins[i] / 255, 0.85);
        // MATH-06: always emit a stop. Skipping low bins lets the gradient
        // interpolate a non-zero colour across sparse zones instead of fading
        // to transparent — emit α=0 there so gaps actually read as empty.
        const a = t > 0.02 ? t * MAX_ALPHA : 0;
        grad.addColorStop(i * step, `rgba(${cr + (hr-cr)*t},${cg + (hg-cg)*t},${cb + (hb-cb)*t},${a})`);
        if (a > 0) hasVisibleBins = true;
      }
      if (hasVisibleBins) {
        ctx.fillStyle = grad;
        ctx.fillRect(xStart, rectY, w, rectH);
      }
    }
  } else if (levels.length) {
    // Fallback: live OB heatmap, без изменений по структуре, но альфа понижена
    const sigma = (mx - mn) * 0.005;
    const STRIDE = 4;
    const sparseH = Math.ceil(physH / STRIDE);
    const sparseDensities = new Float32Array(sparseH);
    let maxD = 0;
    for (let sy = 0; sy < sparseH; sy++) {
      const y = sy * STRIDE;
      const rowP = mn + (1 - y / physH) * (mx - mn);
      let d = 0;
      for (const lv of levels) {
        const diff = (rowP - lv.price) / sigma;
        d += lv.size * Math.exp(-0.5 * diff * diff);
      }
      sparseDensities[sy] = d;
      if (d > maxD) maxD = d;
    }
    if (maxD > 0) {
      const img = ctx.createImageData(physW, physH);
      const logMaxD = Math.log(1 + maxD);
      for (let y = 0; y < physH; y++) {
        const sy = y / STRIDE;
        const lo = Math.floor(sy);
        const hi = Math.min(lo + 1, sparseH - 1);
        const frac = sy - lo;
        const d = sparseDensities[lo] * (1 - frac) + sparseDensities[hi] * frac;
        let t = Math.log(1 + d) / logMaxD;
        t = Math.pow(t, 0.45);
        const r = cr + (hr - cr) * t;
        const g = cg + (hg - cg) * t;
        const b = cb + (hb - cb) * t;
        const a = t * 75; // было 110
        const base = y * physW * 4;
        for (let x = 0; x < physW; x++) {
          const xPct = physW > 1 ? x / (physW - 1) : 1;
          const OB_START = 0.85;
          const fade = xPct < OB_START ? 0 : Math.pow((xPct - OB_START) / (1 - OB_START), 2);
          const off = base + x * 4;
          img.data[off]     = r;
          img.data[off + 1] = g;
          img.data[off + 2] = b;
          img.data[off + 3] = a * fade;
        }
      }
      ctx.putImageData(img, 0, 0);
    }
  }

  if (!levels.length) { ctx.restore(); return; }

  // logMaxD для OB-колонки
  let liveMaxD = 0;
  {
    const sg = (mx - mn) * 0.012;
    for (let i = 0; i <= 40; i++) {
      const rp = mx - (i / 40) * (mx - mn);
      let d = 0;
      for (const lv of levels) { const z = (rp - lv.price) / sg; d += lv.size * Math.exp(-0.5 * z * z); }
      if (d > liveMaxD) liveMaxD = d;
    }
  }
  const logMaxD = Math.log(1 + liveMaxD) || 1;

  // OB-колонка справа, альфа понижена
  ctx.save();
  ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
  {
    const obRows = 40;
    const obColW = Math.floor(W * 0.14);
    const obColX = W - obColW;
    const bestBid = bids.reduce((m, b) => b.price > m ? b.price : m, -Infinity);
    const bestAsk = asks.reduce((m, a) => a.price < m ? a.price : m, Infinity);
    const obMidPrice = isFinite(bestBid) && isFinite(bestAsk) ? (bestBid + bestAsk) / 2 : (mn + mx) / 2;
    const [br2, bg2, bb2] = C.wallBidRgb;
    const [ar2, ag2, ab2] = C.wallAskRgb;
    for (let i = 0; i < obRows; i++) {
      const pct = i / obRows;
      const rowPrice = mx - pct * (mx - mn);
      const y = H - ((rowPrice - mn) / (mx - mn)) * H;
      const rowH = H / obRows + 0.5;
      let d = 0;
      const rowSigma = (mx - mn) * 0.012;
      for (const lv of levels) {
        const diff = (rowPrice - lv.price) / rowSigma;
        d += lv.size * Math.exp(-0.5 * diff * diff);
      }
      const alpha = Math.min(0.40, (Math.log(1 + d) / logMaxD) * 0.36); // было 0.55
      const isBid = rowPrice < obMidPrice;
      ctx.fillStyle = isBid
        ? `rgba(${br2},${bg2},${bb2},${alpha.toFixed(3)})`
        : `rgba(${ar2},${ag2},${ab2},${alpha.toFixed(3)})`;
      ctx.fillRect(obColX, y - rowH / 2, obColW, rowH);
    }
    ctx.strokeStyle = '#0f1a2e';
    ctx.lineWidth = 1;
    ctx.strokeRect(obColX, 0, obColW, H);
  }
  ctx.restore();

  // Walls — тоньше и тише
  ctx.save();
  ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
  const wallT = wallThreshold(levels, ticker);
  const wallMaxSize = levels.reduce((max, lv) => lv.size > max ? lv.size : max, 0);
  const wallLogMax  = Math.log(1 + wallMaxSize) || 1;
  ctx.lineJoin = 'round';

  const drawWalls = (sideLevels: ObLevel[], rgb: readonly [number, number, number]) => {
    const [wr, wg, wb] = rgb;
    for (const lv of sideLevels) {
      if (lv.size >= wallT && lv.price >= mn && lv.price <= mx) {
        const t = Math.log(1 + lv.size) / wallLogMax;
        const alpha = (0.10 + 0.25 * t).toFixed(2);   // было 0.15 + 0.35
        ctx.strokeStyle = `rgba(${wr},${wg},${wb},${alpha})`;
        ctx.lineWidth = 0.4 + 1.6 * t;                 // было 0.5 + 2.5
        const yPos = toY(lv.price, H, mn, mx);
        ctx.beginPath();
        ctx.moveTo(0, yPos);
        ctx.lineTo(W, yPos);
        ctx.stroke();
      }
    }
  };
  drawWalls(bids, C.wallBidRgb);
  drawWalls(asks, C.wallAskRgb);

  // LIVE OB badge
  ctx.font = '8px monospace';
  const liveText = '● LIVE OB';
  const liveW2 = ctx.measureText(liveText).width + 8;
  ctx.fillStyle = 'rgba(26,148,72,0.12)';
  ctx.fillRect(W - liveW2 - 4, 4, liveW2 + 4, 14);
  ctx.fillStyle = '#26c26e';
  ctx.textAlign = 'right';
  ctx.fillText(liveText, W - 6, 14);
  ctx.restore();

  // «Now-focus» — мягкая вертикальная подсветка справа (правые 20%)
  ctx.save();
  ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
  const focusGrad = ctx.createLinearGradient(W * 0.80, 0, W, 0);
  focusGrad.addColorStop(0, 'rgba(255,255,255,0.00)');
  focusGrad.addColorStop(1, 'rgba(216,232,248,0.035)');
  ctx.fillStyle = focusGrad;
  ctx.fillRect(W * 0.80, 0, W * 0.20, H);
  ctx.restore();

  ctx.restore();
}

// ── Draw: Price + Ghost + Spread Shadow (L2) ──────────────────────────────────
function drawPrice(
  canvas: HTMLCanvasElement,
  pts: ChartPoint[],
  mn: number, mx: number,
  lastTs: number, effectiveWindowMs: number,
) {
  const ctx = canvas.getContext('2d');
  if (!ctx || !pts.length) return;
  const C = getChartColors();
  const dpr = getDpr();
  const W   = canvas.width  / dpr;
  const H   = canvas.height / dpr;
  if (W <= 0 || H <= 0) return; // FEAT-04: skip render on zero-size canvas

  ctx.save();
  ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
  ctx.clearRect(0, 0, W, H);
  const gx = makeXOf(lastTs, effectiveWindowMs, W);
  const minVisibleTs = lastTs - effectiveWindowMs;

  const startIdx = findWindowStart(pts, minVisibleTs);
  const visiblePts = pts.slice(startIdx);
  const n = visiblePts.length;
  if (n === 0) { ctx.restore(); return; }

  // MATH-08: ONE price-direction reference for the whole layer. Area fill,
  // glow and the price tag previously each picked their own base (visible
  // window vs full buffer) and could disagree (green line, red tag).
  const vsRefPt  = visiblePts.find(p => p.mid > 0);
  const vsLastPt = findLastValid(visiblePts);
  const vsIsUp   = !!vsLastPt && !!vsRefPt && vsLastPt.mid >= vsRefPt.mid;

  // Grid сначала (под всем) — линии по «круглым» пунктам (1/2/5 × 10^k),
  // а не по равным долям диапазона: цены на оси читаются как тики.
  const yTicks = niceTicks(mn, mx, 5);
  const yStep = yTicks.length > 1 ? yTicks[1] - yTicks[0] : (mx - mn || 1);
  ctx.save();
  for (const price of yTicks) {
    const y = toY(price, H, mn, mx);
    ctx.beginPath();
    ctx.setLineDash([2, 6]);
    ctx.strokeStyle = 'rgba(30,45,65,0.55)';
    ctx.lineWidth = 1;
    ctx.moveTo(0, y);
    ctx.lineTo(W, y);
    ctx.stroke();
    ctx.setLineDash([]);
  }
  ctx.restore();

  // Spread shadow
  ctx.beginPath();
  let shadowStarted = false;
  for (let i = 0; i < n; i++) {
    if (visiblePts[i].mid <= 0) { shadowStarted = false; continue; }
    const hs = (visiblePts[i].spread_bps / 20000) * visiblePts[i].mid;
    const x = gx(visiblePts[i].ts_unix_ms);
    if (!shadowStarted) { ctx.moveTo(x, toY(visiblePts[i].mid + hs, H, mn, mx)); shadowStarted = true; }
    else ctx.lineTo(x, toY(visiblePts[i].mid + hs, H, mn, mx));
  }
  for (let i = n - 1; i >= 0; i--) {
    if (visiblePts[i].mid <= 0) continue;
    const hs = (visiblePts[i].spread_bps / 20000) * visiblePts[i].mid;
    ctx.lineTo(gx(visiblePts[i].ts_unix_ms), toY(visiblePts[i].mid - hs, H, mn, mx));
  }
  ctx.closePath();
  ctx.fillStyle = C.spreadShadow;
  ctx.fill();

  // Area fill — лёгкий, чтобы не съедать акценты
  {
    const firstValidIdx = visiblePts.findIndex(p => p.mid > 0);
    const lastValidPt = findLastValid(visiblePts);
    if (firstValidIdx >= 0 && lastValidPt) {
      const fillClr = vsIsUp ? 'rgba(38,194,110,' : 'rgba(224,80,80,';
      const areaGrad = ctx.createLinearGradient(0, 0, 0, H);
      areaGrad.addColorStop(0, fillClr + '0.07)'); // было 0.12
      areaGrad.addColorStop(0.5, fillClr + '0.02)');
      areaGrad.addColorStop(1, fillClr + '0.00)');

      // MATH-07: build one closed polygon per contiguous run. A gap (mid<=0)
      // must close the current segment down to the baseline, otherwise fill()
      // implicitly joins the last point back to the first across the gap and
      // paints spurious triangles over the chart.
      ctx.beginPath();
      let aStarted = false;
      let aFirstX = 0;
      let aLastX = 0;
      const closeSeg = () => {
        if (!aStarted) return;
        ctx.lineTo(aLastX, H);
        ctx.lineTo(aFirstX, H);
        ctx.closePath();
        aStarted = false;
      };
      for (let i = 0; i < n; i++) {
        if (visiblePts[i].mid <= 0) { closeSeg(); continue; }
        const ax = gx(visiblePts[i].ts_unix_ms);
        const ay = toY(visiblePts[i].mid, H, mn, mx);
        aLastX = ax;
        if (!aStarted) { ctx.moveTo(ax, H); ctx.lineTo(ax, ay); aFirstX = ax; aStarted = true; }
        else ctx.lineTo(ax, ay);
      }
      closeSeg();
      ctx.fillStyle = areaGrad;
      ctx.fill();
    }
  }

  // Ghost — только при corr > CORR_SHOW (0.5)
  const ghostSegs: { x1: number; y1: number; x2: number; y2: number; corr: number }[] = [];
  for (let i = 1; i < n; i++) {
    const pt = visiblePts[i];
    const prev = visiblePts[i - 1];
    if (pt.leader_correlation < CORR_SHOW) continue;
    const targetTs0 = prev.ts_unix_ms - prev.leader_lag_ms;
    const targetTs1 = pt.ts_unix_ms   - pt.leader_lag_ms;
    const idx0 = findClosestByTs(pts, targetTs0, 0, startIdx + i - 1);
    const idx1 = findClosestByTs(pts, targetTs1, 0, startIdx + i);
    const base0 = pts[idx0];
    const base1 = pts[idx1];
    if (base0.mid <= 0 || base1.mid <= 0) continue;
    // FUNC-04: implicit beta = 1. Real beta(target, leader) is often 1.2–3 for
    // alts, so this understates the predicted move. Needs server-supplied beta
    // (or std(target)/std(leader)); left as-is until that field exists.
    // TODO(beta): g = base.mid * (1 + leader_change_1s * correlation * beta)
    const g0 = base0.mid * (1 + prev.leader_change_1s * prev.leader_correlation);
    const g1 = base1.mid * (1 + pt.leader_change_1s   * pt.leader_correlation);
    ghostSegs.push({
      x1: gx(prev.ts_unix_ms), y1: toY(g0, H, mn, mx),
      x2: gx(pt.ts_unix_ms),   y2: toY(g1, H, mn, mx),
      corr: (prev.leader_correlation + pt.leader_correlation) / 2,
    });
  }
  // PERF-06: batch ghost segments by color — two GPU submits instead of N
  if (ghostSegs.length) {
    ctx.save();
    ctx.setLineDash([4, 4]);
    ctx.lineWidth = 1;
    for (const color of [C.ghostHigh, C.ghostMid] as const) {
      ctx.beginPath();
      ctx.strokeStyle = color;
      for (const seg of ghostSegs) {
        const segColor = seg.corr > CORR_HIGH ? C.ghostHigh : C.ghostMid;
        if (segColor !== color) continue;
        ctx.moveTo(seg.x1, seg.y1);
        ctx.lineTo(seg.x2, seg.y2);
      }
      ctx.stroke();
    }
    ctx.restore();
  }

  // Price line (no glow)
  ctx.lineJoin = 'round';
  let pvX: number | null = null;
  let pvY: number | null = null;
  for (let i = 0; i < n; i++) {
    if (visiblePts[i].mid <= 0) { pvX = null; pvY = null; continue; }
    const cx = gx(visiblePts[i].ts_unix_ms);
    const cy = toY(visiblePts[i].mid, H, mn, mx);
    if (pvX !== null && pvY !== null) {
      const agg = visiblePts[i].tape_aggression;
      ctx.beginPath();
      ctx.moveTo(pvX, pvY);
      ctx.lineTo(cx, cy);
      ctx.lineWidth = 1.4 + Math.min(1, Math.abs(agg)) * 2.0;
      ctx.strokeStyle = agg > 0.15 ? '#26c26e' : agg < -0.15 ? '#e05050' : '#a8bccc';
      ctx.stroke();
    }
    pvX = cx; pvY = cy;
  }

  // Current price dot (no pulse animation)
  const lastValidPt = findLastValid(visiblePts);
  if (lastValidPt) {
    const dotAgg = lastValidPt.tape_aggression;
    const curColor = dotAgg > 0.15 ? '#26c26e' : dotAgg < -0.15 ? '#e05050' : '#d8e8f8';
    const lx = gx(lastValidPt.ts_unix_ms);
    const ly = toY(lastValidPt.mid, H, mn, mx);

    ctx.beginPath();
    ctx.arc(lx, ly, 4, 0, Math.PI * 2);
    ctx.fillStyle = curColor;
    ctx.fill();
    ctx.strokeStyle = '#0a0f18';
    ctx.lineWidth = 1.5;
    ctx.stroke();
  }

  // Y-axis labels (тоньше, не перебивают L1)
  ctx.font = '10px monospace';
  ctx.textAlign = 'right';
  ctx.fillStyle = '#5a6e84';
  {
    const dec = decimalsFor(yStep);
    // DISP-02: skip labels closer than the font height so they don't overlap
    // when niceTicks packs many ticks into a short canvas.
    const MIN_TICK_PX = 14;
    let lastLabelY = -Infinity;
    for (const price of yTicks) {
      const y = toY(price, H, mn, mx);
      if (Math.abs(y - lastLabelY) < MIN_TICK_PX) continue;
      ctx.fillText(price.toFixed(dec), W - 6, y + 3);
      lastLabelY = y;
    }
  }

  // Current price tag (правый край) — крупный, контрастный
  if (lastValidPt) {
    const isUp = vsIsUp; // MATH-08: unified visible-window direction
    const curColor = isUp ? '#26c26e' : '#e05050';
    const yLast = toY(lastValidPt.mid, H, mn, mx);

    ctx.save();
    ctx.setLineDash([4, 5]);
    ctx.strokeStyle = curColor + '66';
    ctx.lineWidth = 1;
    ctx.beginPath();
    ctx.moveTo(0, yLast);
    ctx.lineTo(W, yLast);
    ctx.stroke();
    ctx.restore();

    const labelStr = fmtPrice(lastValidPt.mid);
    ctx.font = 'bold 12px monospace';
    const textMetrics = ctx.measureText(labelStr);
    const labelW = textMetrics.width + 14;
    const labelH = 20;

    ctx.fillStyle = isUp ? '#1a332a' : '#331a1a';
    ctx.beginPath();
    if (typeof ctx.roundRect === 'function') {
      ctx.roundRect(W - labelW - 4, yLast - labelH/2, labelW + 4, labelH, [4, 0, 0, 4]);
    } else {
      ctx.rect(W - labelW - 4, yLast - labelH/2, labelW + 4, labelH);
    }
    ctx.fill();
    ctx.strokeStyle = curColor;
    ctx.lineWidth = 1;
    ctx.stroke();

    ctx.fillStyle = '#fff';
    ctx.textAlign = 'right';
    ctx.textBaseline = 'middle';
    ctx.fillText(labelStr, W - 8, yLast);
    ctx.textBaseline = 'alphabetic';
  }

  // Time axis — DISP-04: ticks snapped to round wall-clock instants so they
  // stay anchored while panning instead of sliding with the right edge.
  ctx.font = '9px monospace';
  ctx.fillStyle = '#3c4e62';
  ctx.textAlign = 'center';
  {
    const firstTs = lastTs - effectiveWindowMs;
    const stepMs = niceTimeStepMs(effectiveWindowMs, 5);
    for (let t = Math.ceil(firstTs / stepMs) * stepMs; t <= lastTs; t += stepMs) {
      const x = (1 - (lastTs - t) / effectiveWindowMs) * W;
      if (x < 12 || x > W - 12) continue;
      const timeStr = new Date(t).toLocaleTimeString('en-US', { hour: '2-digit', minute: '2-digit', hour12: false });
      ctx.fillText(timeStr, x, H - 4);
    }
  }

  ctx.restore();
}

// ── Draw: POSITION LAYER (L1) ────────────────────────────────────────────────
// Открытые позиции (open_trades): entry-линия, SL-линия, TP-линия, R/R-зоны, PnL pill.
// Закрытые сделки (journal): entry-точка, exit-точка по cause_of_exit, соединитель.

function drawPosition(
  canvas: HTMLCanvasElement,
  pts: ChartPoint[],
  openTrades: Trade[],
  journal: JournalEntry[],
  ticker: string,
  mn: number, mx: number,
  lastTs: number, windowMs: number,
) {
  const ctx = canvas.getContext('2d');
  if (!ctx) return;
  const C = getChartColors();
  const dpr = getDpr();
  const W   = canvas.width  / dpr;
  const H   = canvas.height / dpr;
  if (W <= 0 || H <= 0) return; // FEAT-04: skip render on zero-size canvas

  ctx.save();
  ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
  ctx.clearRect(0, 0, W, H);

  if (!pts.length) { ctx.restore(); return; }
  const gx = makeXOf(lastTs, windowMs, W);

  const tickerShort = (s: string) => (s.split(':')[1] ?? s).replace('.p','').replace('.m','');
  const T = tickerShort(ticker);

  // ── Закрытые сделки журнала ────────────────────────────────────────────────
  const visibleJournal = journal
    .filter(j => tickerShort(j.ticker ?? '') === T || !j.ticker)
    .filter(j => j.tsMs >= lastTs - windowMs - 5000 && j.tsMs <= lastTs + 5000)
    .slice(-20);

  for (const j of visibleJournal) {
    if (!j.entryPrice || !j.exitPrice) continue;
    const isLong = sideIsLong(j.side);
    const entryTs = (j.entryTsMs ?? j.tsMs - 1000);
    const exitTs  = j.tsMs;
    const x1 = gx(entryTs);
    const x2 = gx(exitTs);
    const y1 = toY(j.entryPrice, H, mn, mx);
    const y2 = toY(j.exitPrice, H, mn, mx);

    // соединитель
    const pnl = j.pnlUsd ?? 0;
    const pnlRgb = pnl >= 0 ? C.causeTpRgb : C.causeSlRgb;
    const intensity = Math.min(1, Math.abs(pnl) / 5); // нормировка
    ctx.beginPath();
    ctx.moveTo(x1, y1);
    ctx.lineTo(x2, y2);
    ctx.strokeStyle = `rgba(${pnlRgb[0]},${pnlRgb[1]},${pnlRgb[2]},${(0.35 + 0.45 * intensity).toFixed(2)})`;
    ctx.lineWidth = 1 + intensity * 1.5;
    ctx.stroke();

    // entry — кольцо
    const [er, eg, eb] = isLong ? C.entryLongRgb : C.entryShortRgb;
    ctx.beginPath();
    ctx.arc(x1, y1, 3.5, 0, Math.PI * 2);
    ctx.fillStyle = `rgba(${er},${eg},${eb},0.95)`;
    ctx.fill();
    ctx.strokeStyle = '#0a0f18';
    ctx.lineWidth = 1.2;
    ctx.stroke();

    // exit — цвет по cause
    const cause = (j.causeOfExit ?? '').toUpperCase();
    const causeRgb =
      cause === 'TP' || cause.startsWith('TP') ? C.causeTpRgb :
      cause === 'SL' || cause.startsWith('SL') ? C.causeSlRgb :
      cause === 'SIGNAL' ? C.causeSignalRgb :
      cause === 'TIME' || cause === 'TIMEOUT' ? C.causeTimeRgb :
      pnl >= 0 ? C.causeTpRgb : C.causeSlRgb;

    ctx.beginPath();
    ctx.arc(x2, y2, 4.5, 0, Math.PI * 2);
    ctx.fillStyle = `rgba(${causeRgb[0]},${causeRgb[1]},${causeRgb[2]},1)`;
    ctx.fill();
    ctx.strokeStyle = '#0a0f18';
    ctx.lineWidth = 1.5;
    ctx.stroke();

    // мини-метка причины
    ctx.font = '8px monospace';
    ctx.fillStyle = `rgba(${causeRgb[0]},${causeRgb[1]},${causeRgb[2]},0.9)`;
    ctx.textAlign = 'center';
    ctx.fillText(cause || (pnl >= 0 ? 'WIN' : 'LOSS'), x2, y2 - 7);
  }

  // ── Открытые позиции (highest priority — поверх всего) ─────────────────────
  const openInTicker = openTrades.filter(t => tickerShort(t.ticker ?? '') === T);

  for (const trade of openInTicker) {
    const entry = trade.entryPrice;
    const sl    = trade.stopLoss;
    const tp    = trade.takeProfit;
    if (!entry || entry <= 0) continue;

    const isLong = sideIsLong(trade.side);
    const [er, eg, eb] = isLong ? C.entryLongRgb : C.entryShortRgb;

    const yEntry = toY(entry, H, mn, mx);

    // R/R зоны (заливка)
    if (sl && sl > 0) {
      const ySL = toY(sl, H, mn, mx);
      ctx.fillStyle = `rgba(${C.slRgb[0]},${C.slRgb[1]},${C.slRgb[2]},0.07)`;
      ctx.fillRect(0, Math.min(yEntry, ySL), W, Math.abs(ySL - yEntry));
    }
    if (tp && tp > 0) {
      const yTP = toY(tp, H, mn, mx);
      ctx.fillStyle = `rgba(${C.tpRgb[0]},${C.tpRgb[1]},${C.tpRgb[2]},0.07)`;
      ctx.fillRect(0, Math.min(yEntry, yTP), W, Math.abs(yTP - yEntry));
    }

    // ENTRY линия + подпись
    ctx.save();
    ctx.strokeStyle = `rgba(${er},${eg},${eb},0.95)`;
    ctx.lineWidth = 2;
    ctx.shadowColor = `rgba(${er},${eg},${eb},0.6)`;
    ctx.shadowBlur = 6;
    ctx.beginPath();
    ctx.moveTo(0, yEntry);
    ctx.lineTo(W, yEntry);
    ctx.stroke();
    ctx.restore();

    // Подпись слева: LONG 0.012 @ 0.0604
    const sideTxt = isLong ? 'LONG' : 'SHORT';
    const sizeTxt = trade.executedSize != null ? trade.executedSize.toFixed(3) : (trade.sizeCoin ?? 0).toFixed(3);
    const entryLbl = `${sideTxt} ${sizeTxt} @ ${fmtPrice(entry)}`;
    ctx.font = 'bold 10px monospace';
    const tw = ctx.measureText(entryLbl).width + 10;
    ctx.fillStyle = '#0a0f18';
    ctx.fillRect(2, yEntry - 8, tw, 16);
    ctx.strokeStyle = `rgba(${er},${eg},${eb},0.9)`;
    ctx.lineWidth = 1;
    ctx.strokeRect(2, yEntry - 8, tw, 16);
    ctx.fillStyle = '#fff';
    ctx.textAlign = 'left';
    ctx.textBaseline = 'middle';
    ctx.fillText(entryLbl, 7, yEntry);
    ctx.textBaseline = 'alphabetic';

    // Strategy в правом краю на уровне entry
    if (trade.strategy) {
      ctx.font = '9px monospace';
      const stwx = ctx.measureText(trade.strategy).width + 8;
      ctx.fillStyle = 'rgba(10,15,24,0.85)';
      ctx.fillRect(W - stwx - 60, yEntry - 7, stwx, 14);
      ctx.fillStyle = `rgba(${er},${eg},${eb},1)`;
      ctx.textAlign = 'left';
      ctx.textBaseline = 'middle';
      ctx.fillText(trade.strategy, W - stwx - 56, yEntry);
      ctx.textBaseline = 'alphabetic';
    }

    // PnL pill — у правого края, чуть ниже/выше entry
    const pnl = trade.unrealizedPnl;
    if (pnl != null) {
      const pnlClr: readonly [number,number,number] = pnl >= 0 ? C.tpRgb : C.slRgb;
      const pnlTxt = (pnl >= 0 ? '+$' : '-$') + Math.abs(pnl).toFixed(2);
      ctx.font = 'bold 11px monospace';
      const pw = ctx.measureText(pnlTxt).width + 12;
      // DISP-03: keep the 18px pill inside the canvas when entry is near an edge
      const py = Math.max(2, Math.min(H - 20, yEntry + (isLong ? -30 : 14)));
      ctx.fillStyle = `rgba(${pnlClr[0]},${pnlClr[1]},${pnlClr[2]},0.18)`;
      ctx.fillRect(W - pw - 56, py, pw, 18);
      ctx.strokeStyle = `rgba(${pnlClr[0]},${pnlClr[1]},${pnlClr[2]},0.9)`;
      ctx.lineWidth = 1;
      ctx.strokeRect(W - pw - 56, py, pw, 18);
      ctx.fillStyle = `rgba(${pnlClr[0]},${pnlClr[1]},${pnlClr[2]},1)`;
      ctx.textAlign = 'left';
      ctx.textBaseline = 'middle';
      ctx.fillText(pnlTxt, W - pw - 50, py + 9);
      ctx.textBaseline = 'alphabetic';
    }

    // SL линия
    if (sl && sl > 0) {
      const ySL = toY(sl, H, mn, mx);
      // MATH-03: express as P&L direction, not raw price direction (SHORT inverts)
      const slPct = ((sl - entry) / entry) * 100 * (isLong ? 1 : -1);
      ctx.save();
      ctx.setLineDash([4, 3]);
      ctx.strokeStyle = `rgba(${C.slRgb[0]},${C.slRgb[1]},${C.slRgb[2]},0.85)`;
      ctx.lineWidth = 1.2;
      ctx.beginPath();
      ctx.moveTo(0, ySL);
      ctx.lineTo(W, ySL);
      ctx.stroke();
      ctx.restore();

      const slLbl = `SL ${fmtPrice(sl)} · ${slPct >= 0 ? '+' : ''}${slPct.toFixed(2)}%`;
      ctx.font = '10px monospace';
      const slw = ctx.measureText(slLbl).width + 10;
      ctx.fillStyle = '#1a0a0a';
      ctx.fillRect(W - slw - 56, ySL - 7, slw, 14);
      ctx.strokeStyle = `rgba(${C.slRgb[0]},${C.slRgb[1]},${C.slRgb[2]},0.9)`;
      ctx.lineWidth = 1;
      ctx.strokeRect(W - slw - 56, ySL - 7, slw, 14);
      ctx.fillStyle = `rgba(${C.slRgb[0]},${C.slRgb[1]},${C.slRgb[2]},1)`;
      ctx.textAlign = 'left';
      ctx.textBaseline = 'middle';
      ctx.fillText(slLbl, W - slw - 51, ySL);
      ctx.textBaseline = 'alphabetic';
    }

    // TP линия
    if (tp && tp > 0) {
      const yTP = toY(tp, H, mn, mx);
      // MATH-03: P&L direction (SHORT inverts)
      const tpPct = ((tp - entry) / entry) * 100 * (isLong ? 1 : -1);
      ctx.save();
      ctx.setLineDash([4, 3]);
      ctx.strokeStyle = `rgba(${C.tpRgb[0]},${C.tpRgb[1]},${C.tpRgb[2]},0.85)`;
      ctx.lineWidth = 1.2;
      ctx.beginPath();
      ctx.moveTo(0, yTP);
      ctx.lineTo(W, yTP);
      ctx.stroke();
      ctx.restore();

      const tpLbl = `TP ${fmtPrice(tp)} · ${tpPct >= 0 ? '+' : ''}${tpPct.toFixed(2)}%`;
      ctx.font = '10px monospace';
      const tw2 = ctx.measureText(tpLbl).width + 10;
      ctx.fillStyle = '#0a1a10';
      ctx.fillRect(W - tw2 - 56, yTP - 7, tw2, 14);
      ctx.strokeStyle = `rgba(${C.tpRgb[0]},${C.tpRgb[1]},${C.tpRgb[2]},0.9)`;
      ctx.lineWidth = 1;
      ctx.strokeRect(W - tw2 - 56, yTP - 7, tw2, 14);
      ctx.fillStyle = `rgba(${C.tpRgb[0]},${C.tpRgb[1]},${C.tpRgb[2]},1)`;
      ctx.textAlign = 'left';
      ctx.textBaseline = 'middle';
      ctx.fillText(tpLbl, W - tw2 - 51, yTP);
      ctx.textBaseline = 'alphabetic';
    }

    // Distance-to-SL / Distance-to-TP — две вертикальные шкалы на правом краю
    if (sl && tp && sl > 0 && tp > 0) {
      const lastPt = findLastValid(pts);
      if (lastPt) {
        const curMid = lastPt.mid;
        // прогресс: 0 у entry, 1 у уровня
        const slProg = Math.max(0, Math.min(1, (entry - curMid) / (entry - sl)));
        const tpProg = Math.max(0, Math.min(1, (curMid - entry) / (tp - entry)));
        const barX = W - 44;
        const barY = 4;
        const barH = H - 8;
        // фон
        ctx.fillStyle = 'rgba(20,30,45,0.6)';
        ctx.fillRect(barX, barY, 4, barH);
        ctx.fillRect(barX + 8, barY, 4, barH);
        // SL прогресс — снизу вверх
        ctx.fillStyle = `rgba(${C.slRgb[0]},${C.slRgb[1]},${C.slRgb[2]},0.85)`;
        ctx.fillRect(barX, barY + barH * (1 - slProg), 4, barH * slProg);
        // TP прогресс — сверху вниз
        ctx.fillStyle = `rgba(${C.tpRgb[0]},${C.tpRgb[1]},${C.tpRgb[2]},0.85)`;
        ctx.fillRect(barX + 8, barY, 4, barH * tpProg);
        // подписи
        ctx.font = '7px monospace';
        ctx.fillStyle = '#5a6e84';
        ctx.textAlign = 'center';
        ctx.fillText('SL', barX + 2, barY + barH + 7);
        ctx.fillText('TP', barX + 10, barY + barH + 7);
      }
    }
  }

  ctx.restore();
}

// ── Draw: Signal arrows (приглушены) ─────────────────────────────────────────
function drawSignals(
  canvas: HTMLCanvasElement,
  pts: ChartPoint[],
  mn: number, mx: number,
  signals: { timestamp: number; price?: number; side: string }[],
  lastTs: number, windowMs: number,
) {
  const ctx = canvas.getContext('2d');
  if (!ctx || !pts.length) return;
  const C = getChartColors();
  const dpr = getDpr();
  const W   = canvas.width  / dpr;
  const H   = canvas.height / dpr;
  if (W <= 0 || H <= 0) return; // FEAT-04: skip render on zero-size canvas

  ctx.save();
  ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
  ctx.clearRect(0, 0, W, H);

  if (!signals.length) { ctx.restore(); return; }
  const ox = makeXOf(lastTs, windowMs, W);

  for (const sig of signals) {
    if (!sig.price) continue;
    const t = sig.timestamp;
    if (t < lastTs - windowMs - 5000 || t > lastTs + 5000) continue;
    const x = ox(t);
    const y = toY(sig.price, H, mn, mx);
    const isBuy = sig.side === 'BUY' || sig.side === 'LONG';
    const sz = 5;
    ctx.beginPath();
    ctx.fillStyle = isBuy ? C.sigBuy : C.sigSell;
    if (isBuy) {
      ctx.moveTo(x, y - sz);
      ctx.lineTo(x + sz * 0.7, y + sz * 0.4);
      ctx.lineTo(x - sz * 0.7, y + sz * 0.4);
    } else {
      ctx.moveTo(x, y + sz);
      ctx.lineTo(x + sz * 0.7, y - sz * 0.4);
      ctx.lineTo(x - sz * 0.7, y - sz * 0.4);
    }
    ctx.closePath();
    ctx.fill();
  }
  ctx.restore();
}

// ── Draw: Iceberg pulses (как было) ──────────────────────────────────────────
function drawIceberg(
  canvas: HTMLCanvasElement,
  pts: ChartPoint[],
  mn: number, mx: number,
  events: IcebergEvent[],
  now: number,
  lastTs: number, windowMs: number,
) {
  const ctx = canvas.getContext('2d');
  if (!ctx || !pts.length) return;
  const C = getChartColors();
  const dpr = getDpr();
  const W   = canvas.width  / dpr;
  const H   = canvas.height / dpr;
  if (W <= 0 || H <= 0) return; // FEAT-04: skip render on zero-size canvas

  ctx.save();
  ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
  ctx.clearRect(0, 0, W, H);

  if (!events.length) { ctx.restore(); return; }
  const ix = makeXOf(lastTs, windowMs, W);
  const PULSE_MS = 5000;
  const [ir, ig, ib] = C.icebergRgb;

  for (const ev of events) {
    const age = now - ev.ts_ms;
    if (age < 0 || age > PULSE_MS) continue;
    const alpha = 1 - age / PULSE_MS;
    const radius = (age / PULSE_MS) * 30;
    const x = ix(ev.ts_ms);
    const y = toY(ev.price, H, mn, mx);
    ctx.beginPath();
    ctx.arc(x, y, radius, 0, Math.PI * 2);
    ctx.strokeStyle = `rgba(${ir},${ig},${ib},${alpha.toFixed(2)})`;
    ctx.lineWidth = 1.5;
    ctx.stroke();
  }
  ctx.restore();
}

// ── Draw: Flow Ribbon (тише) ─────────────────────────────────────────────────
function drawRibbon(canvas: HTMLCanvasElement, pts: ChartPoint[], lastTs: number, windowMs: number, ticker: string) {
  const ctx = canvas.getContext('2d');
  if (!ctx || !pts.length) return;
  const C = getChartColors();
  const dpr = getDpr();
  const W   = canvas.width  / dpr;
  const H   = canvas.height / dpr;
  if (W <= 0 || H <= 0) return; // FEAT-04: skip render on zero-size canvas

  ctx.save();
  ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
  ctx.clearRect(0, 0, W, H);
  const rx = makeXOf(lastTs, windowMs, W);
  const minVisibleTs = lastTs - windowMs;

  const startIdx = findWindowStart(pts, minVisibleTs);
  const visiblePts = pts.slice(startIdx);
  const n = visiblePts.length;
  if (n === 0) { ctx.restore(); return; }

  const halfH = H / 2;
  const maxVol = ribbonPerc95(pts, startIdx, ticker);
  const [br, bg, bb] = C.buyRibbon;
  const [sr, sg, sb] = C.sellRibbon;

  ctx.strokeStyle = 'rgba(60,78,98,0.4)';
  ctx.lineWidth = 1;
  ctx.beginPath(); ctx.moveTo(0, halfH); ctx.lineTo(W, halfH); ctx.stroke();

  for (let i = 0; i < n; i++) {
    const pt = visiblePts[i];
    const endX = rx(pt.ts_unix_ms);
    const prevTs = i === 0 ? pt.ts_unix_ms - TICK_MS : visiblePts[i - 1].ts_unix_ms;
    const startX = rx(prevTs);
    const barW = Math.max(1, Math.min(Math.ceil(endX - startX), Math.ceil(W / 60)));

    const buyH  = Math.min(halfH, (pt.buy_vol_5s  / maxVol) * halfH);
    const sellH = Math.min(halfH, (pt.sell_vol_5s / maxVol) * halfH);
    const agg   = pt.tape_aggression;
    const buyA  = pt.buy_vol_5s  > 0 ? (0.08 + Math.max(0, agg)  * 0.55).toFixed(2) : '0.04';
    const sellA = pt.sell_vol_5s > 0 ? (0.08 + Math.max(0, -agg) * 0.55).toFixed(2) : '0.04';

    ctx.fillStyle = `rgba(${br},${bg},${bb},${buyA})`;
    ctx.fillRect(startX, halfH - buyH, barW, buyH);
    ctx.fillStyle = `rgba(${sr},${sg},${sb},${sellA})`;
    ctx.fillRect(startX, halfH, barW, sellH);
  }
  ctx.restore();
}

function hasValidPrices(pts: ChartPoint[]) { return pts.some(p => p.mid > 0); }

const shortName = (t: string) => (t.split(':')[1] ?? t).replace('.p', '').replace('.m', '');

interface PriceMeta {
  price: number; isUp: boolean; changePct: number; spreadBps: number;
  aggression: number; buyVol: number; sellVol: number; cvd: number; ageMs: number;
}

function computeMeta(pts: ChartPoint[]): PriceMeta | null {
  const b = sanePriceBounds(pts);
  if (!b) return null;
  const { first, last } = b;
  // MATH-01: buy_vol_5s / sell_vol_5s are trailing-5s rolling sums. Summing the
  // raw value over every ~100ms point counts each trade ~50× (50 overlapping
  // windows per 5s). Accumulate only the positive increment between adjacent
  // points (window growth ≈ fresh flow); skip ticks where the window reset
  // (negative delta) or price is invalid.
  let cvd = 0;
  let prev: ChartPoint | null = null;
  for (const p of pts) {
    if (p.mid <= 0) { prev = null; continue; }
    if (prev) {
      const dBuy  = p.buy_vol_5s  - prev.buy_vol_5s;
      const dSell = p.sell_vol_5s - prev.sell_vol_5s;
      if (dBuy >= 0 && dSell >= 0) cvd += dBuy - dSell;
    }
    prev = p;
  }
  return {
    price: last.mid,
    isUp: last.mid >= first.mid,
    changePct: ((last.mid - first.mid) / first.mid) * 100,
    spreadBps: last.spread_bps,
    aggression: last.tape_aggression,
    buyVol: last.buy_vol_5s,
    sellVol: last.sell_vol_5s,
    cvd,
    ageMs: Date.now() - last.ts_unix_ms,
  };
}

// ── Component ─────────────────────────────────────────────────────────────────  // TYPE-03
  interface PriceUpdateDetail { price: number; ageMs: number; isUp: boolean; changePct: number; }

  // Sub-component for price and latency to isolate DOM updates
  const PriceDisplay = React.memo(({ ticker }: { ticker: string }) => {
    const priceRef = useRef<HTMLSpanElement>(null);
    const statusRef = useRef<HTMLSpanElement>(null);
    const statusDotRef = useRef<HTMLSpanElement>(null);
    const ageTextRef = useRef<HTMLSpanElement>(null);
    const changeRef = useRef<HTMLSpanElement>(null);

    useEffect(() => {
      const handleUpdate = (e: CustomEvent<PriceUpdateDetail>) => {
      const { price, ageMs, isUp, changePct } = e.detail;
      if (priceRef.current) {
        priceRef.current.innerText = fmtPrice(price);
        priceRef.current.style.color = isUp ? '#26c26e' : '#e05050';
      }
      if (changeRef.current) {
        changeRef.current.style.color = isUp ? '#26c26e' : '#e05050';
        changeRef.current.style.background = isUp ? 'rgba(38,194,110,0.12)' : 'rgba(224,80,80,0.12)';
        changeRef.current.innerText = (changePct >= 0 ? '+' : '') + changePct.toFixed(2) + '%';
      }
      
      const fresh = ageMs < 2000;
      const stale = ageMs > 10000;
      const c = stale ? '#e05050' : fresh ? '#26c26e' : '#c08828';
      
      if (statusRef.current) {
        statusRef.current.style.color = c;
        statusRef.current.style.background = c + '1f';
        statusRef.current.title = `Data feed latency: ${ageMs}ms`;
      }
      if (statusDotRef.current) {
        statusDotRef.current.style.background = c;
        statusDotRef.current.className = 'w-1.5 h-1.5 rounded-full inline-block' + (fresh ? ' animate-pulse' : '');
      }
      if (ageTextRef.current) {
        ageTextRef.current.innerText = (stale ? 'STALE ' : fresh ? 'LIVE ' : 'LAG ') + (ageMs / 1000).toFixed(1) + 's';
      }
    };

    window.addEventListener(`chart-update-${ticker}`, handleUpdate);
    return () => window.removeEventListener(`chart-update-${ticker}`, handleUpdate);
  }, [ticker]);

  return (
    <div className="flex items-center gap-3">
      <span className="font-mono text-[12px] font-semibold text-white tracking-wide">
        {(ticker.split(':')[1] ?? ticker).replace('.p','')}
      </span>
      <span ref={priceRef} className="font-mono font-bold" style={{ fontSize: 18, lineHeight: 1 }}>—</span>
      <span ref={changeRef} className="font-mono text-[10px] px-1.5 py-0.5 rounded">—</span>
      <span ref={statusRef} className="font-mono text-[9px] px-1.5 py-0.5 rounded flex items-center gap-1">
        <span ref={statusDotRef} className="w-1.5 h-1.5 rounded-full inline-block" />
        <span ref={ageTextRef}>—</span>
      </span>
    </div>
  );
});

// FEAT-01: Error Boundary — a NaN coordinate or divide-by-zero in a draw
// function must not unmount the whole chart. Falls back to a placeholder
// and auto-recovers when the ticker changes.
class ChartErrorBoundary extends React.Component<
  { ticker: string; children: React.ReactNode },
  { hasError: boolean }
> {
  state = { hasError: false };
  static getDerivedStateFromError() { return { hasError: true }; }
  componentDidUpdate(prev: { ticker: string }) {
    if (prev.ticker !== this.props.ticker && this.state.hasError) {
      this.setState({ hasError: false });
    }
  }
  render() {
    if (this.state.hasError) {
      return (
        <div className="w-full h-full flex items-center justify-center bg-[#080c16]">
          <div className="flex flex-col items-center gap-1.5">
            <div className="w-1.5 h-1.5 bg-[#e05050] rounded-full" />
            <span className="font-mono text-[9px] text-[#e05050] tracking-[.12em] uppercase">
              Chart render error
            </span>
          </div>
        </div>
      );
    }
    return this.props.children;
  }
}

function AdvancedChartInner({ ticker }: { ticker: string }) {
  const orderbook      = useTradeStore(s => s.orderbook);
  const signals        = useTradeStore(s => s.signals);
  const icebergEvents  = useTradeStore(s => s.icebergEvents);
  const densityHistory = useTradeStore(s => s.densityHistory);
  const trades         = useTradeStore(s => s.trades);
  const journalEntries = useTradeStore(s => s.journalEntries);
  
  // PERF-02: Get mutable buffer for direct reading in RAF
  const bufferRef = useRef(marketDataService.getBuffer(ticker));
  
  // Update buffer reference when ticker changes
  useEffect(() => {
    bufferRef.current = marketDataService.getBuffer(ticker);
  }, [ticker]);

  const yEmaRef = useRef<{ cur: Range | null }>({ cur: null });
  const resolvedRangeRef = useRef<Range | null>(null);
  const containerRef = useRef<HTMLDivElement>(null);
  const densityRef   = useRef<HTMLCanvasElement>(null);
  const priceCanvasRef = useRef<HTMLCanvasElement>(null); // ARCH-03: renamed from priceRef
  const positionRef  = useRef<HTMLCanvasElement>(null);
  const overlayRef   = useRef<HTMLCanvasElement>(null);
  const icebergRef   = useRef<HTMLCanvasElement>(null);
  const ribbonRef    = useRef<HTMLCanvasElement>(null);

  // PERF-01: dirty flags — only redraw layers whose data actually changed
  const dirtyRef = useRef({ density: true, price: true, position: true, ribbon: true, signals: true });

  const [priceMeta, setPriceMeta] = useState<PriceMeta | null>(null);
  const [regimeMeta, setRegimeMeta] = useState<{ regime: Regime; vol: number; imb: number; agg: number }>(
    { regime: 'NEUTRAL', vol: 0, imb: 0, agg: 0 }
  );

  const [crosshair, setCrosshair] = useState<{ x: number; y: number } | null>(null);
  const mainChartRef = useRef<HTMLDivElement>(null);

  const DIVIDER_H = 6;
  const [splitPct, setSplitPct] = useState(0.78);
  const splitPctRef = useRef(0.78);

  const syncSize = useCallback(() => {
    const el = containerRef.current;
    if (!el) return;
    const W = el.clientWidth || 1;
    const totalH = el.clientHeight || 1;
    const mainH = Math.max(20, Math.floor(totalH * splitPctRef.current));
    const ribH  = Math.max(20, totalH - mainH - DIVIDER_H);
    const dpr = getDpr();
    for (const r of [densityRef, priceCanvasRef, positionRef, overlayRef, icebergRef]) {
      if (r.current) {
        r.current.width  = W * dpr;
        r.current.height = mainH * dpr;
        r.current.style.width  = W + 'px';
        r.current.style.height = mainH + 'px';
      }
    }
    if (ribbonRef.current) {
      ribbonRef.current.width  = W * dpr;
      ribbonRef.current.height = ribH * dpr;
      ribbonRef.current.style.width  = W + 'px';
      ribbonRef.current.style.height = ribH + 'px';
    }
  }, []);

  const dragTeardownRef = useRef<(() => void) | null>(null);
  useEffect(() => () => dragTeardownRef.current?.(), []);
  useEffect(() => { splitPctRef.current = splitPct; }, [splitPct]);

  const handleDividerMouseDown = useCallback((e: React.MouseEvent) => {
    e.preventDefault();
    const el = containerRef.current;
    if (!el) return;
    const rect = el.getBoundingClientRect();
    const startY = e.clientY;
    const startPct = splitPctRef.current;
    const totalH = rect.height - DIVIDER_H;

    const onMove = (e: MouseEvent) => {
      const dy = e.clientY - startY;
      const newPct = Math.min(0.92, Math.max(0.08, startPct + dy / totalH));
      splitPctRef.current = newPct;
      syncSize();
      setSplitPct(newPct);
    };
    const onUp = () => {
      document.removeEventListener('mousemove', onMove);
      document.removeEventListener('mouseup', onUp);
      dragTeardownRef.current = null;
    };
    dragTeardownRef.current = () => {
      document.removeEventListener('mousemove', onMove);
      document.removeEventListener('mouseup', onUp);
    };
    document.addEventListener('mousemove', onMove);
    document.addEventListener('mouseup', onUp);
  }, [syncSize]);

  const handleDividerTouchStart = useCallback((e: React.TouchEvent) => {
    e.preventDefault();
    const el = containerRef.current;
    if (!el) return;
    const rect = el.getBoundingClientRect();
    const startY = e.touches[0].clientY;
    const startPct = splitPctRef.current;
    const totalH = rect.height - DIVIDER_H;

    const onMove = (e: TouchEvent) => {
      const dy = e.touches[0].clientY - startY;
      const newPct = Math.min(0.92, Math.max(0.08, startPct + dy / totalH));
      splitPctRef.current = newPct;
      syncSize();
      setSplitPct(newPct);
    };
    const onUp = () => {
      document.removeEventListener('touchmove', onMove);
      document.removeEventListener('touchend', onUp);
      dragTeardownRef.current = null;
    };
    dragTeardownRef.current = () => {
      document.removeEventListener('touchmove', onMove);
      document.removeEventListener('touchend', onUp);
    };
    document.addEventListener('touchmove', onMove);
    document.addEventListener('touchend', onUp);
  }, [syncSize]);

  const handleChartMouseMove = useCallback((e: React.MouseEvent<HTMLDivElement>) => {
    const rect = e.currentTarget.getBoundingClientRect();
    setCrosshair({ x: e.clientX - rect.left, y: e.clientY - rect.top });
  }, []);
  const handleChartMouseLeave = useCallback(() => setCrosshair(null), []);

  const viewRef = useRef({ windowMs: 0, rightTs: 0 });
  useEffect(() => {
    const el = mainChartRef.current;
    if (!el) return;
    const onWheel = (e: WheelEvent) => {
      e.preventDefault();
      const rect = el.getBoundingClientRect();
      const W = rect.width || 1;
      const fx = Math.min(1, Math.max(0, (e.clientX - rect.left) / W));
      // PERF-02: Read from mutable buffer
      const pts = bufferRef.current.chart.toArray();
      if (!pts || !pts.length) return;
      const { lastTs, windowMs } = xWindow(pts, viewRef.current.windowMs, viewRef.current.rightTs);
      const tsCursor = lastTs - (1 - fx) * windowMs;
      const k = Math.exp(-e.deltaY * 0.0012);
      const newWin = Math.min(TIME_WINDOW_MS, Math.max(MIN_WINDOW_MS, windowMs / k));
      let newRight = tsCursor + (1 - fx) * newWin;
      const now = Date.now();
      if (newRight >= now) {
        viewRef.current = { windowMs: newWin, rightTs: 0 };
      } else {
        const firstTs = pts.find(p => p.mid > 0)?.ts_unix_ms ?? tsCursor;
        newRight = Math.max(newRight, firstTs + newWin * 0.15);
        viewRef.current = { windowMs: newWin, rightTs: newRight };
      }
    };
    // ARCH-04: horizontal drag-pan (TradingView-style)
    let dragging = false;
    let dragStartX = 0;
    let dragStartRight = 0;
    let dragWindowMs = 0;
    const onDown = (e: MouseEvent) => {
      if (e.button !== 0) return;
      // PERF-02: Read from mutable buffer
      const pts = bufferRef.current.chart.toArray();
      if (!pts || !pts.length) return;
      const { lastTs, windowMs } = xWindow(pts, viewRef.current.windowMs, viewRef.current.rightTs);
      dragging = true;
      dragStartX = e.clientX;
      dragStartRight = lastTs;
      dragWindowMs = windowMs;
      el.style.cursor = 'grabbing';
    };
    const onDrag = (e: MouseEvent) => {
      if (!dragging) return;
      const rect = el.getBoundingClientRect();
      const W = rect.width || 1;
      const dx = e.clientX - dragStartX;
      // drag right → pan into the past → rightTs decreases
      let newRight = dragStartRight - (dx / W) * dragWindowMs;
      // PERF-02: Read from mutable buffer
      const pts = bufferRef.current.chart.toArray();
      const now = Date.now();
      if (newRight >= now) {
        viewRef.current = { windowMs: dragWindowMs, rightTs: 0 };
      } else {
        const firstTs = pts?.find(p => p.mid > 0)?.ts_unix_ms ?? newRight;
        newRight = Math.max(newRight, firstTs + dragWindowMs * 0.15);
        viewRef.current = { windowMs: dragWindowMs, rightTs: newRight };
      }
    };
    const onUp = () => {
      if (!dragging) return;
      dragging = false;
      el.style.cursor = '';
    };
    el.addEventListener('wheel', onWheel, { passive: false });
    el.addEventListener('mousedown', onDown);
    window.addEventListener('mousemove', onDrag);
    window.addEventListener('mouseup', onUp);
    return () => {
      el.removeEventListener('wheel', onWheel);
      el.removeEventListener('mousedown', onDown);
      window.removeEventListener('mousemove', onDrag);
      window.removeEventListener('mouseup', onUp);
    };
  }, []);
  const handleChartDoubleClick = useCallback(() => {
    viewRef.current = { windowMs: 0, rightTs: 0 };
  }, []);

  useEffect(() => {
    syncSize();
    const ro = new ResizeObserver(syncSize);
    if (containerRef.current) ro.observe(containerRef.current);
    return () => ro.disconnect();
  }, [syncSize]);

  const filteredSignals = useMemo(() => {
    const st = shortName(ticker);
    return signals.filter(s => shortName(s.ticker) === st);
  }, [signals, ticker]);

  const openTradesForTicker = useMemo(() => {
    const st = shortName(ticker);
    return (trades ?? []).filter(t => shortName(t.ticker ?? '') === st);
  }, [trades, ticker]);

  const liveRef = useRef({
    orderbook, icebergEvents, densityHistory, filteredSignals,
    openTrades: openTradesForTicker, journal: journalEntries ?? [], ticker,
    lastChartTs: 0, // Track last chart point timestamp for dirty flag
  });
  
  // PERF-02: set dirty flags when data changes (chartHistory removed)
  React.useLayoutEffect(() => {
    liveRef.current = {
      orderbook, icebergEvents, densityHistory, filteredSignals,
      openTrades: openTradesForTicker, journal: journalEntries ?? [], ticker,
      lastChartTs: liveRef.current.lastChartTs,
    };
    dirtyRef.current.density = true;
    dirtyRef.current.price = true;
    dirtyRef.current.signals = true;
    dirtyRef.current.ribbon = true;
  }, [orderbook, icebergEvents, densityHistory, filteredSignals, openTradesForTicker, journalEntries, ticker]);

  React.useLayoutEffect(() => {
    dirtyRef.current.position = true;
  }, [openTradesForTicker]);

  useEffect(() => {
    yEmaRef.current.cur = null;
    resolvedRangeRef.current = null;
    setPriceMeta(null);
    // D2-FIX: Reset dirty flags on ticker switch to prevent stale data render
    dirtyRef.current = { density: true, price: true, position: true, ribbon: true, signals: true };
    // CACHE-FIX: Invalidate all caches on ticker switch
    _wallThreshCache = null;
    _ribbonP95Cache = null;
    _lastRegimeChange = 0;
    lastRegimeRef.current = 'NEUTRAL';
  }, [ticker]);

  // BUG-03: throttle CustomEvent dispatch via last-dispatched price ref
  const lastDispatchedPriceRef = useRef(0);

  // PERF-01: track last regime to avoid redundant setState calls
  const lastRegimeRef = useRef<Regime>('NEUTRAL');

  useEffect(() => {
    let rafId = 0;
    let lastMetaPush = 0;
    const animate = () => {
      const {
        orderbook, icebergEvents, densityHistory,
        filteredSignals, openTrades, journal, ticker: currentTicker
      } = liveRef.current;

      // PERF-02: Read chart data from mutable buffer
      const chartBuffer = bufferRef.current.chart;
      const chartHistory = chartBuffer.toArray(); // Legacy compatibility - TODO: refactor helpers to use buffer directly

      if (chartHistory.length) {
        // D4-FIX: Reduce pulse threshold to 1s to avoid false "live" on stale data
        // REFRESH-FIX: Force redraw every 500ms even if no new data to prevent freeze
        let freshTs = 0;
        for (let i = chartHistory.length - 1; i >= 0; i--) {
          if (chartHistory[i].mid > 0) { freshTs = chartHistory[i].ts_unix_ms; break; }
        }
        const now = Date.now();
        const timeSinceLastDraw = now - (lastMetaPush || 0);
        if ((freshTs && now - freshTs < 1000) || timeSinceLastDraw > 500) {
          dirtyRef.current.price = true;
          dirtyRef.current.density = true;
          dirtyRef.current.position = true;
        }

        // #13: nothing changed, feed stale, no iceberg pulse active →
        // skip the per-frame range math and all draws this frame.
        const anyDirty = dirtyRef.current.density || dirtyRef.current.price ||
          dirtyRef.current.position || dirtyRef.current.ribbon || dirtyRef.current.signals;
        if (!anyDirty && icebergEvents.length === 0) {
          rafId = requestAnimationFrame(animate);
          return;
        }

        const extras: number[] = [];
        for (const t of openTrades) {
          if (t.entryPrice) extras.push(t.entryPrice);
          if (t.stopLoss)   extras.push(t.stopLoss);
          if (t.takeProfit) extras.push(t.takeProfit);
        }

        const rawRange = priceRange(chartHistory, extras);
        const range = smoothRange(rawRange, yEmaRef.current);
        resolvedRangeRef.current = range;

        const { lastTs, windowMs } = xWindow(chartHistory, viewRef.current.windowMs, viewRef.current.rightTs);

        // PERF-01: draw only dirty layers
        if (dirtyRef.current.density && densityRef.current)
          drawDensity(densityRef.current, orderbook.bids, orderbook.asks,
            densityHistory, lastTs, windowMs, range.min, range.max, currentTicker);
        if (dirtyRef.current.price && priceCanvasRef.current)
          drawPrice(priceCanvasRef.current, chartHistory, range.min, range.max, lastTs, windowMs);
        if (dirtyRef.current.position && positionRef.current)
          drawPosition(positionRef.current, chartHistory, openTrades, journal, currentTicker, range.min, range.max, lastTs, windowMs);
        if (dirtyRef.current.ribbon && ribbonRef.current)
          drawRibbon(ribbonRef.current, chartHistory, lastTs, windowMs, currentTicker);
        if (dirtyRef.current.signals && overlayRef.current)
          drawSignals(overlayRef.current, chartHistory, range.min, range.max, filteredSignals, lastTs, windowMs);
        if (icebergRef.current)
          drawIceberg(icebergRef.current, chartHistory, range.min, range.max, icebergEvents, Date.now(), lastTs, windowMs);

        // Reset dirty flags after drawing
        dirtyRef.current.density = false;
        dirtyRef.current.price = false;
        dirtyRef.current.position = false;
        dirtyRef.current.ribbon = false;
        dirtyRef.current.signals = false;

        let lp = 0, lts = 0;
        for (let i = chartHistory.length - 1; i >= 0; i--) {
          if (chartHistory[i].mid > 0) { lp = chartHistory[i].mid; lts = chartHistory[i].ts_unix_ms; break; }
        }

        // D9-FIX: Dispatch price immediately, throttle only regime updates
        if (lp > 0 && lp !== lastDispatchedPriceRef.current) {
          lastDispatchedPriceRef.current = lp;
          const vStart = findWindowStart(chartHistory, lastTs - windowMs);
          let firstVisible: ChartPoint | undefined;
          for (let i = vStart; i < chartHistory.length; i++) {
            if (chartHistory[i].mid > 0) { firstVisible = chartHistory[i]; break; }
          }
          const baseMid = firstVisible?.mid ?? lp;
          const changePct = baseMid > 0 ? ((lp - baseMid) / baseMid) * 100 : 0;
          window.dispatchEvent(new CustomEvent<PriceUpdateDetail>(`chart-update-${currentTicker}`, {
            detail: {
              price: lp,
              ageMs: Date.now() - lts,
              isUp: lp >= baseMid,
              changePct
            }
          }));
          // Update price meta immediately
          const m = computeMeta(chartHistory);
          if (m) setPriceMeta(m);
        }

        // Regime updates only every 500ms (less critical)
        const perfNow = performance.now();
        if (perfNow - lastMetaPush > 500) {
          lastMetaPush = perfNow;
          const newRegime = classifyRegime(chartHistory, lastRegimeRef.current);
          if (newRegime.regime !== lastRegimeRef.current) {
            lastRegimeRef.current = newRegime.regime;
            setRegimeMeta(newRegime);
          }
        }
      }
      rafId = requestAnimationFrame(animate);
    };
    rafId = requestAnimationFrame(animate);
    return () => cancelAnimationFrame(rafId);
  }, []);

  const priceColor = priceMeta ? (priceMeta.isUp ? '#26c26e' : '#e05050') : '#7a8fa8';
  const C = useMemo(() => getChartColors(), [regimeMeta.regime, priceMeta?.isUp]);

  const activeTrade = openTradesForTicker[0];
  // BUG-05: show position count indicator when multiple positions are open
  const extraPositionCount = openTradesForTicker.length - 1;

  // D8-FIX: Memoize by crosshair coordinates and trade ID, not full objects
  const crosshairInfo = useMemo(() => {
    if (!crosshair || !mainChartRef.current) return null;
    const chartHistory = bufferRef.current.chart.toArray();
    if (!chartHistory.length) return null;
    const el = mainChartRef.current;
    const w = el.clientWidth || 1;
    const h = el.clientHeight || 1;
    const range = resolvedRangeRef.current ?? priceRange(chartHistory);
    const price = range.min + (1 - crosshair.y / h) * (range.max - range.min);
    const { lastTs, windowMs } = xWindow(chartHistory, viewRef.current.windowMs, viewRef.current.rightTs);
    const ts = lastTs - (1 - crosshair.x / w) * windowMs;
    let nearest = chartHistory[0];
    if (chartHistory.length > 0) {
      const firstValidIdx = chartHistory.findIndex(p => p.mid > 0);
      let lastValidIdx = -1;
      for (let i = chartHistory.length - 1; i >= 0; i--) {
        if (chartHistory[i].mid > 0) { lastValidIdx = i; break; }
      }
      if (firstValidIdx >= 0 && lastValidIdx >= firstValidIdx) {
        const idx = findClosestByTs(chartHistory, ts, firstValidIdx, lastValidIdx);
        nearest = chartHistory[idx];
      }
    }
    const deltas: { label: string; value: string; color: string }[] = [];
    if (activeTrade?.entryPrice) {
      const d = ((price - activeTrade.entryPrice) / activeTrade.entryPrice) * 100;
      deltas.push({
        label: 'Δ entry',
        value: (d >= 0 ? '+' : '') + d.toFixed(2) + '%',
        color: d >= 0 ? '#3cdc8c' : '#f05a5a',
      });
    }
    if (activeTrade?.stopLoss) {
      const d = ((price - activeTrade.stopLoss) / activeTrade.stopLoss) * 100;
      deltas.push({ label: '→ SL', value: (d >= 0 ? '+' : '') + d.toFixed(2) + '%', color: '#f05a5a' });
    }
    if (activeTrade?.takeProfit) {
      const d = ((price - activeTrade.takeProfit) / activeTrade.takeProfit) * 100;
      deltas.push({ label: '→ TP', value: (d >= 0 ? '+' : '') + d.toFixed(2) + '%', color: '#3cdc8c' });
    }
    return {
      price,
      time: new Date(ts).toLocaleTimeString('en-US', { hour12: false }),
      agg: nearest?.tape_aggression ?? 0,
      deltas,
    };
  }, [crosshair?.x, crosshair?.y, activeTrade?.id, activeTrade?.entryPrice, activeTrade?.stopLoss, activeTrade?.takeProfit]);

  const regimeClr = regimeColor(regimeMeta.regime, C);
  const chartHistory = bufferRef.current.chart.toArray();

  return (
    <div className="w-full h-full flex flex-col select-none">
      {/* ── HEADER ── */}
      <div className="shrink-0 flex flex-col border-b border-[#0f1a2e] bg-[#080c16]">
        {/* Row 1 — price + status (isolated in sub-component) */}
        <div className="flex items-center gap-3 px-3 py-1.5">
          <PriceDisplay ticker={ticker} />

          <div className="flex-1" />

          {/* Regime pill */}
          <span
            className="font-mono text-[10px] font-semibold px-2 py-0.5 rounded uppercase tracking-wider"
            style={{ color: regimeClr, background: regimeClr + '1f', border: `1px solid ${regimeClr}55` }}
            title={`vol ${regimeMeta.vol.toFixed(1)}bps · imb ${regimeMeta.imb.toFixed(2)} · agg ${regimeMeta.agg.toFixed(2)}`}
          >
            {regimeMeta.regime}
          </span>

          {priceMeta && !activeTrade && (<>
            <div className="flex flex-col gap-0">
              <span className="font-mono text-[8px] text-[#3c4e62] uppercase tracking-wider">Spread</span>
              <span className="font-mono text-[10px] text-[#7a8fa8]">{priceMeta.spreadBps.toFixed(1)} bps</span>
            </div>
            <div className="flex flex-col gap-0">
              <span className="font-mono text-[8px] text-[#3c4e62] uppercase tracking-wider">Aggr</span>
              <span
                className="font-mono text-[10px]"
                style={{ color: priceMeta.aggression > 0.1 ? '#26c26e' : priceMeta.aggression < -0.1 ? '#e05050' : '#7a8fa8' }}
              >
                {(priceMeta.aggression >= 0 ? '+' : '') + priceMeta.aggression.toFixed(2)}
              </span>
            </div>
            <div className="flex flex-col gap-0">
              <span className="font-mono text-[8px] text-[#3c4e62] uppercase tracking-wider">CVD</span>
              <span
                className="font-mono text-[10px]"
                style={{ color: priceMeta.cvd > 0 ? '#26c26e' : priceMeta.cvd < 0 ? '#e05050' : '#7a8fa8' }}
              >
                {(priceMeta.cvd >= 0 ? '+' : '') + priceMeta.cvd.toFixed(0)}
              </span>
            </div>
          </>)}
        </div>

        {/* Row 2 — позиция (только когда открыта) */}
        {activeTrade && (() => {
          const isLong = sideIsLong(activeTrade.side);
          const sideClr = isLong ? '#3cdc8c' : '#f05a5a';
          const entry = activeTrade.entryPrice ?? 0;
          const sl = activeTrade.stopLoss;
          const tp = activeTrade.takeProfit;
          const pnl = activeTrade.unrealizedPnl;
          const pnlClr = pnl == null ? '#7a8fa8' : pnl >= 0 ? '#3cdc8c' : '#f05a5a';
          // MATH-03: P&L direction, not raw price direction (SHORT inverts)
          const dir = isLong ? 1 : -1;
          const slPct = sl ? ((sl - entry) / entry) * 100 * dir : null;
          const tpPct = tp ? ((tp - entry) / entry) * 100 * dir : null;
          const sizeTxt = activeTrade.executedSize != null
            ? activeTrade.executedSize.toFixed(3)
            : (activeTrade.sizeCoin ?? 0).toFixed(3);

          return (
            <div className="flex items-center gap-3 px-3 py-1 border-t border-[#0f1a2e] bg-[#0a1018]">
              <span
                className="font-mono text-[10px] font-bold px-1.5 py-0.5 rounded uppercase"
                style={{ color: '#0a0f18', background: sideClr }}
              >
                {isLong ? 'LONG' : 'SHORT'}
              </span>
              <span className="font-mono text-[11px] text-white">
                {sizeTxt} <span className="text-[#7a8fa8]">@</span> {fmtPrice(entry)}
              </span>
              {pnl != null && (
                <span className="font-mono text-[11px] font-bold" style={{ color: pnlClr }}>
                  PnL {(pnl >= 0 ? '+$' : '-$') + Math.abs(pnl).toFixed(2)}
                </span>
              )}
              <div className="h-3 w-px bg-[#1a2d41]" />
              {sl && slPct != null && (
                <span className="font-mono text-[10px]" style={{ color: '#f05a5a' }}>
                  SL {fmtPrice(sl)} <span className="opacity-70">({slPct >= 0 ? '+' : ''}{slPct.toFixed(2)}%)</span>
                </span>
              )}
              {tp && tpPct != null && (
                <span className="font-mono text-[10px]" style={{ color: '#3cdc8c' }}>
                  TP {fmtPrice(tp)} <span className="opacity-70">({tpPct >= 0 ? '+' : ''}{tpPct.toFixed(2)}%)</span>
                </span>
              )}
              {/* BUG-05: position count badge when multiple positions */}
              {extraPositionCount > 0 && (
                <span className="font-mono text-[9px] px-1.5 py-0.5 rounded bg-[#1a2d41] text-[#7a8fa8]">
                  +{extraPositionCount} more
                </span>
              )}
              {/* DATA-01: visual indicator when position data is incomplete.
                   If the store doesn't emit SL/TP/entry fields the drawPosition
                   layer silently skips them — this badge tells the operator the
                   data is missing, not that there are no positions. */}
              {(!activeTrade.stopLoss || !activeTrade.takeProfit || !activeTrade.entryPrice) && (
                <span
                  className="font-mono text-[9px] px-1.5 py-0.5 rounded bg-[#c08828]/10 border border-[#c08828]/30 text-[#c08828]"
                  title="Position data incomplete — SL/TP may not render on chart"
                >
                  ⚠ PARTIAL
                </span>
              )}
              <div className="flex-1" />
              {activeTrade.strategy && (
                <span className="font-mono text-[10px] text-[#a8bccc] uppercase tracking-wider">
                  {activeTrade.strategy}
                </span>
              )}
            </div>
          );
        })()}
      </div>

      <div ref={containerRef} className="flex-1 min-h-0 flex flex-col">
        {/* Main chart */}
        <div
          ref={mainChartRef}
          className="relative cursor-grab"
          style={{ height: `${(splitPct * 100).toFixed(1)}%` }}
          onMouseMove={handleChartMouseMove}
          onMouseLeave={handleChartMouseLeave}
          onDoubleClick={handleChartDoubleClick}
          title="Wheel: zoom time · Drag: pan · Double-click: reset"
        >
          <canvas ref={densityRef}  className="absolute inset-0" style={{ width: '100%', height: '100%' }} />
          <canvas ref={priceCanvasRef} className="absolute inset-0" style={{ width: '100%', height: '100%' }} />
          <canvas ref={positionRef} className="absolute inset-0" style={{ width: '100%', height: '100%' }} />
          <canvas ref={icebergRef}  className="absolute inset-0" style={{ width: '100%', height: '100%' }} />
          <canvas ref={overlayRef}  className="absolute inset-0" style={{ width: '100%', height: '100%' }} />

          {!hasValidPrices(chartHistory) && (
            <div className="absolute inset-0 flex items-center justify-center pointer-events-none z-10">
              <div className="flex flex-col items-center gap-1.5">
                <div className="w-1.5 h-1.5 bg-[#3c4e62] rounded-full" />
                <span className="font-mono text-[9px] text-[#3c4e62] tracking-[.12em] uppercase">Waiting for price data</span>
              </div>
            </div>
          )}

          {/* Data Quality Indicators */}
          {(() => {
            if (!chartHistory.length) return null;
            let lastValidTs = 0;
            for (let i = chartHistory.length - 1; i >= 0; i--) {
              if (chartHistory[i].mid > 0) { lastValidTs = chartHistory[i].ts_unix_ms; break; }
            }
            if (!lastValidTs) return null;
            
            const ageMs = Date.now() - lastValidTs;
            const isStale = ageMs > 5000;
            const isWarning = ageMs > 2000 && ageMs <= 5000;
            
            // Detect gaps (missing data points > 500ms)
            let gapCount = 0;
            for (let i = 1; i < chartHistory.length; i++) {
              const prev = chartHistory[i - 1];
              const curr = chartHistory[i];
              if (prev.mid > 0 && curr.mid > 0 && curr.ts_unix_ms - prev.ts_unix_ms > 500) {
                gapCount++;
              }
            }
            
            if (!isStale && !isWarning && gapCount === 0) return null;
            
            return (
              <div className="absolute top-2 right-2 flex flex-col gap-1 pointer-events-none z-30">
                {(isStale || isWarning) && (
                  <div className={cn(
                    "px-2 py-1 rounded font-mono text-[9px] font-bold border",
                    isStale ? "bg-[#ae2c2c]/10 border-[#ae2c2c]/30 text-[#ae2c2c]" : "bg-[#c08828]/10 border-[#c08828]/30 text-[#c08828]"
                  )}>
                    {isStale ? '⚠ STALE' : '⚠ LAG'} {(ageMs / 1000).toFixed(1)}s
                  </div>
                )}
                {gapCount > 0 && (
                  <div className="px-2 py-1 rounded font-mono text-[9px] font-bold border bg-[#c08828]/10 border-[#c08828]/30 text-[#c08828]">
                    ⚠ {gapCount} GAP{gapCount > 1 ? 'S' : ''}
                  </div>
                )}
              </div>
            );
          })()}

          {crosshair && (
            <div className="absolute inset-0 pointer-events-none z-20">
              <div className="absolute top-0 bottom-0 w-px bg-[#3c4e62]/50" style={{ left: crosshair.x }} />
              <div className="absolute left-0 right-0 h-px bg-[#3c4e62]/50" style={{ top: crosshair.y }} />
              {crosshairInfo && (
                <div
                  className="absolute bg-[rgba(7,11,19,0.95)] border border-[#1a2d41] px-2 py-1 font-mono text-[9px] leading-tight whitespace-nowrap"
                  style={{
                    left: Math.min(crosshair.x + 10, (mainChartRef.current?.clientWidth ?? 0) - 140),
                    top: Math.max(crosshair.y - 60, 2),
                  }}
                >
                  <div className="text-[#d8e8f8] font-semibold">{fmtPrice(crosshairInfo.price)}</div>
                  <div className="text-[#3c4e62]">{crosshairInfo.time}</div>
                  <div style={{ color: crosshairInfo.agg > 0.1 ? '#26c26e' : crosshairInfo.agg < -0.1 ? '#e05050' : '#7a8fa8' }}>
                    aggr {(crosshairInfo.agg >= 0 ? '+' : '') + crosshairInfo.agg.toFixed(2)}
                  </div>
                  {crosshairInfo.deltas.map(d => (
                    <div key={d.label} style={{ color: d.color }}>
                      {d.label} {d.value}
                    </div>
                  ))}
                </div>
              )}
            </div>
          )}
        </div>

        {/* Divider */}
        <div
          className="shrink-0 cursor-row-resize bg-[#0c1220] hover:bg-[#c08828] transition-colors group relative"
          style={{ height: DIVIDER_H }}
          onMouseDown={handleDividerMouseDown}
          onTouchStart={handleDividerTouchStart}
        >
          <div className="absolute inset-x-0 top-1/2 -translate-y-1/2 h-px bg-[#1a2d41] group-hover:bg-[#c08828] transition-colors" />
        </div>

        {/* Ribbon */}
        <div className="relative flex-1 min-h-0 border-t border-[#0c1220]">
          <canvas ref={ribbonRef} className="absolute inset-0" style={{ width: '100%', height: '100%' }} />
          <div className="absolute top-1 left-2 pointer-events-none">
            <span className="font-mono text-[9px] text-[#3c4e62] uppercase tracking-wider">Buy / Sell Flow</span>
          </div>
        </div>

        {/* Legend — colors derived from the SAME palette the canvas draws with
            (was hardcoded and disagreed with the rendered density/walls). */}
        <div className="shrink-0 flex items-center gap-x-2.5 gap-y-1 px-3 py-1.5 border-t border-[#0f1a2e] flex-wrap overflow-hidden">
          {(() => {
            const cs = (a: readonly number[]) => `rgb(${a[0]},${a[1]},${a[2]})`;
            const densGrad = `linear-gradient(90deg, ${cs(C.densityRgb.cold)} 0%, ${cs(C.densityRgb.hot)} 100%)`;
            return [
              { color: cs(C.entryLongRgb),  label: 'Entry L', h: 2 },
              { color: cs(C.entryShortRgb), label: 'Entry S', h: 2 },
              { color: cs(C.slRgb), label: 'SL', h: 2, dashed: true },
              { color: cs(C.tpRgb), label: 'TP', h: 2, dashed: true },
              { color: '#26c26e', label: 'Up', h: 2 },
              { color: '#e05050', label: 'Down', h: 2 },
              { color: cs(C.wallBidRgb), label: 'Wall bid', h: 2, w: 12 },
              { color: cs(C.wallAskRgb), label: 'Wall ask', h: 2, w: 12 },
              { color: densGrad, label: 'Density', h: 6, w: 12, raw: true },
            ];
          })().map(({ color, label, h, w, dashed, raw }: any) => (
            <div key={label} className="flex items-center gap-1 shrink-0">
              <div
                style={{
                  width: w ?? 8, height: h,
                  background: raw ? color : dashed ? `repeating-linear-gradient(90deg, ${color} 0 3px, transparent 3px 5px)` : color,
                  borderRadius: 1, flexShrink: 0,
                }}
              />
              <span className="font-mono text-[9px] text-[#3c4e62]">{label}</span>
            </div>
          ))}
        </div>
      </div>
    </div>
  );
}

export function AdvancedChart(props: { ticker: string }) {
  return (
    <ChartErrorBoundary ticker={props.ticker}>
      <AdvancedChartInner ticker={props.ticker} />
    </ChartErrorBoundary>
  );
}
