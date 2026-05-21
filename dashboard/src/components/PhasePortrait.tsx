import React, { useEffect, useRef, useState } from 'react';
import { useTradeStore } from '../store/useTradeStore';

// Thresholds aligned with AdvancedChart's classifyRegime() to avoid
// contradictory regime hints between PhasePortrait and the chart pill.
const VOL_THRESHOLD = 8;
const IMB_THRESHOLD = 0.25;
const AGG_THRESHOLD = 0.20;
const TRAIL_LEN     = 120;
const VOL_MAX       = 20;

// Hysteresis — held across renders so the pill doesn't flicker when
// metrics sit right on the boundary. Mirrors AdvancedChart's logic.
let _prevRegimePhase = 'NEUTRAL';

function getRegime(vol: number, imb: number, agg: number): string {
  const raw = _rawRegime(vol, imb, agg);
  if (raw !== _prevRegimePhase) {
    const stillPrev = _rawRegime(vol * 0.8, imb * 0.8, agg * 0.8);
    if (stillPrev === _prevRegimePhase) return _prevRegimePhase;
  }
  _prevRegimePhase = raw;
  return raw;
}

function _rawRegime(vol: number, imb: number, agg: number): string {
  if (vol >= VOL_THRESHOLD && (agg >= AGG_THRESHOLD || imb >= IMB_THRESHOLD))  return 'MOMENTUM';
  if (vol >= VOL_THRESHOLD && (agg <= -AGG_THRESHOLD || imb <= -IMB_THRESHOLD)) return 'FLUSH';
  if (vol < VOL_THRESHOLD  && imb <= -IMB_THRESHOLD) return 'DISTRIB';
  if (vol < VOL_THRESHOLD  && imb >= IMB_THRESHOLD)  return 'ACCUM';
  return 'NEUTRAL';
}

function getStrategy(regime: string): string {
  switch (regime) {
    case 'MOMENTUM': return 'Breakout';
    case 'FLUSH':    return 'FlushRev';
    case 'DISTRIB':
    case 'ACCUM':    return 'Bounce';
    default:         return 'Wait';
  }
}

const REGIME_COLOR: Record<string, string> = {
  MOMENTUM: '#1a9448',
  FLUSH:    '#ae2c2c',
  DISTRIB:  '#ae2c2c',
  ACCUM:    '#1a9448',
  NEUTRAL:  '#3c4e62',
};

export function PhasePortrait() {
  const chartHistory = useTradeStore(s => s.chartHistory);
  const canvasRef    = useRef<HTMLCanvasElement>(null);
  const [size, setSize] = useState({ w: 0, h: 0 });

  useEffect(() => {
    const canvas = canvasRef.current;
    if (!canvas) return;
    const ro = new ResizeObserver(entries => {
      const r = entries[0];
      if (r) setSize({ w: Math.floor(r.contentRect.width), h: Math.floor(r.contentRect.height) });
    });
    ro.observe(canvas);
    return () => ro.disconnect();
  }, []);

  const last   = chartHistory[chartHistory.length - 1];
  const vol    = last?.volatility_1min_bps ?? 0;
  const imb    = last?.imbalance ?? 0;
  const agg    = last?.tape_aggression ?? 0;
  const regime = last ? getRegime(vol, imb, agg) : 'NEUTRAL';

  useEffect(() => {
    const canvas = canvasRef.current;
    if (!canvas || !chartHistory.length) return;
    const ctx = canvas.getContext('2d');
    if (!ctx) return;

    const dpr = window.devicePixelRatio || 1;
    const W = size.w || canvas.clientWidth || 180;
    const H = size.h || canvas.clientHeight || 160;
    canvas.width  = W * dpr;
    canvas.height = H * dpr;
    canvas.style.width  = W + 'px';
    canvas.style.height = H + 'px';
    ctx.scale(dpr, dpr);

    ctx.clearRect(0, 0, W, H);
    ctx.fillStyle = '#070b13';
    ctx.fillRect(0, 0, W, H);

    const pad  = 28;   // left/bottom margin for axis labels
    const padR = 8;
    const padT = 8;
    const plotW = W - pad - padR;
    const plotH = H - pad - padT;

    const toX = (imb: number) => pad + ((imb + 1) / 2) * plotW;
    const toY = (v: number)   => padT + plotH - (Math.min(v, VOL_MAX) / VOL_MAX) * plotH;

    const midX = toX(0);
    const midY = toY(VOL_THRESHOLD);

    // Quadrant backgrounds
    const quads: [number, number, number, number, string][] = [
      [midX, padT,   pad + plotW - midX, midY - padT,      'rgba(26,148,72,0.04)'],  // MOMENTUM
      [pad,  padT,   midX - pad,         midY - padT,      'rgba(174,44,44,0.04)'],  // FLUSH
      [pad,  midY,   midX - pad,         padT + plotH - midY, 'rgba(174,44,44,0.03)'], // DISTRIB
      [midX, midY,   pad + plotW - midX, padT + plotH - midY, 'rgba(26,148,72,0.03)'], // ACCUM
    ];
    for (const [x, y, w, h, fill] of quads) {
      ctx.fillStyle = fill;
      ctx.fillRect(x, y, w, h);
    }

    // Quadrant dividers
    ctx.strokeStyle = 'rgba(60,78,98,0.5)';
    ctx.lineWidth = 1;
    ctx.setLineDash([3, 4]);
    ctx.beginPath(); ctx.moveTo(midX, padT);      ctx.lineTo(midX, padT + plotH); ctx.stroke();
    ctx.beginPath(); ctx.moveTo(pad, midY);        ctx.lineTo(pad + plotW, midY);  ctx.stroke();
    ctx.setLineDash([]);

    // Quadrant labels
    ctx.font = 'bold 9px monospace';
    const labelPad = 4;
    const labels: [string, string, number, number, string][] = [
      ['MOMEN-', 'TUM',  midX + labelPad, padT + 10,      '#1a9448'],
      ['FLUSH', '',      pad + labelPad,  padT + 10,      '#ae2c2c'],
      ['DISTR', '',      pad + labelPad,  padT + plotH - 4, '#ae2c2c'],
      ['ACCUM', '',      midX + labelPad, padT + plotH - 4, '#1a9448'],
    ];
    for (const [l1, l2, lx, ly, col] of labels) {
      ctx.fillStyle = col + '55';  // dim when not active
      ctx.fillText(l1, lx, ly);
      if (l2) ctx.fillText(l2, lx, ly + 9);
    }

    // Highlight active quadrant label
    ctx.font = 'bold 8px monospace';
    if (regime === 'MOMENTUM') { ctx.fillStyle = '#1a9448aa'; ctx.fillText('MOMEN-', midX + labelPad, padT + 10); ctx.fillText('TUM', midX + labelPad, padT + 19); }
    if (regime === 'FLUSH')    { ctx.fillStyle = '#ae2c2caa'; ctx.fillText('FLUSH', pad + labelPad, padT + 10); }
    if (regime === 'DISTRIB')  { ctx.fillStyle = '#ae2c2caa'; ctx.fillText('DISTR', pad + labelPad, padT + plotH - 4); }
    if (regime === 'ACCUM')    { ctx.fillStyle = '#1a9448aa'; ctx.fillText('ACCUM', midX + labelPad, padT + plotH - 4); }

    // Axis labels
    ctx.font = '8px monospace';
    ctx.fillStyle = '#3c4e62';
    ctx.textAlign = 'center';
    ctx.fillText('IMB →', pad + plotW / 2, padT + plotH + 18);
    ctx.save();
    ctx.translate(10, padT + plotH / 2);
    ctx.rotate(-Math.PI / 2);
    ctx.fillText('VOL ↑', 0, 0);
    ctx.restore();

    // Axis tick values
    ctx.font = '8px monospace';
    ctx.fillStyle = '#2a3a4e';
    ctx.textAlign = 'center';
    for (const v of [-1, -0.5, 0, 0.5, 1]) {
      ctx.fillText(v.toFixed(1), toX(v), padT + plotH + 8);
    }
    ctx.textAlign = 'right';
    for (const v of [0, VOL_THRESHOLD, VOL_MAX]) {
      ctx.fillText(String(v), pad - 2, toY(v) + 3);
    }

    // Trail
    const trail = chartHistory.slice(-TRAIL_LEN);
    for (let i = 0; i < trail.length; i++) {
      const pt = trail[i];
      const alpha = (i / trail.length) * 0.55;
      const radius = Math.max(1.5, Math.min(3.5, (pt.prints_per_sec / 30) * 3.5));
      ctx.beginPath();
      ctx.arc(toX(pt.imbalance), toY(pt.volatility_1min_bps), radius, 0, Math.PI * 2);
      ctx.fillStyle = `rgba(180,200,220,${alpha.toFixed(2)})`;
      ctx.fill();
    }

    // Current point — amber with glow
    if (last) {
      const cx = toX(last.imbalance);
      const cy = toY(last.volatility_1min_bps);
      ctx.beginPath();
      ctx.arc(cx, cy, 9, 0, Math.PI * 2);
      ctx.strokeStyle = 'rgba(192,136,40,0.2)';
      ctx.lineWidth = 4;
      ctx.stroke();
      ctx.beginPath();
      ctx.arc(cx, cy, 5, 0, Math.PI * 2);
      ctx.fillStyle = REGIME_COLOR[regime] ?? '#c08828';
      ctx.fill();
      ctx.beginPath();
      ctx.arc(cx, cy, 2, 0, Math.PI * 2);
      ctx.fillStyle = '#ffffff';
      ctx.fill();
    }

    ctx.resetTransform();
  }, [chartHistory, regime, last, size]);

  const regColor = REGIME_COLOR[regime] ?? '#3c4e62';

  return (
    <div className="border-b border-[#0c1220] px-2.5 py-1.5">
      <div className="flex justify-between items-center mb-1.5">
        <div className="flex items-center gap-1.5">
          <span className="text-[8px] font-semibold text-[#3c4e62] uppercase tracking-[.1em]">Phase Portrait</span>
          <span className="text-[8px] text-[#3c4e62]">vol/imb</span>
        </div>
        <div className="flex items-center gap-2">
          <span className="font-mono text-[8px] text-[#3c4e62]">
            v<span className="text-[#7a8fa8]">{(last?.volatility_1min_bps ?? 0).toFixed(1)}</span>
            {' '}i<span className="text-[#7a8fa8]">{(last?.imbalance ?? 0).toFixed(2)}</span>
          </span>
          <span className="font-mono text-[8px] font-bold" style={{ color: regColor }}>
            {regime}
          </span>
        </div>
      </div>
      <canvas ref={canvasRef} className="w-full" style={{ height: '160px', display: 'block' }} />
      <div className="mt-1 flex justify-between items-center">
        <span className="font-mono text-[8px] text-[#3c4e62]">Strategy hint:</span>
        <span
          className="font-mono text-[9px] font-bold px-2 py-0.5 rounded border"
          style={{
            color: regColor,
            borderColor: regColor + '40',
            backgroundColor: regColor + '15',
          }}
          title={`Current regime: ${regime}. Suggested approach: ${getStrategy(regime)}`}
        >
          {getStrategy(regime)}
        </span>
      </div>
    </div>
  );
}
