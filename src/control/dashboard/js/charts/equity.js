'use strict';

// @depends-on: canvas.js

/**
 * Equity chart renderer.
 * Phase 5: Extract renderEquityChart.
 */
function renderEquityChart(data) {
  const canvas = $('eq-chart');
  if (!canvas) return;
  const { ctx, w, h } = setupCanvas(canvas);
  const history = data.equity_history || [];
  if (history.length < 2) {
    ctx.fillStyle = '#7b859c';
    ctx.font = '10px sans-serif';
    ctx.textAlign = 'center';
    ctx.fillText('No equity history', w / 2, h / 2);
    return;
  }

  const pad = { top: 10, right: 40, bottom: 20, left: 10 },
    pw = w - pad.left - pad.right,
    ph = h - pad.top - pad.bottom;
  const vals = history.map((p) => p.equity);
  const min = Math.min(...vals),
    max = Math.max(...vals),
    range = max - min || 1;
  const toX = (i) => pad.left + (i / (vals.length - 1)) * pw;
  const toY = (v) => pad.top + ph - ((v - min) / range) * ph;

  drawGrid(ctx, w, h, pad);

  // Line
  ctx.beginPath();
  ctx.moveTo(toX(0), toY(vals[0]));
  vals.forEach((v, i) => ctx.lineTo(toX(i), toY(v)));
  ctx.strokeStyle = 'var(--accent)';
  ctx.lineWidth = 1.5;
  ctx.stroke();

  // Area
  ctx.lineTo(toX(vals.length - 1), h - pad.bottom);
  ctx.lineTo(toX(0), h - pad.bottom);
  ctx.fillStyle = 'rgba(99,102,241,0.1)';
  ctx.fill();

  // Labels
  ctx.fillStyle = '#7b859c';
  ctx.font = '8px Inter, sans-serif';
  ctx.textAlign = 'right';
  ctx.fillText(max.toFixed(0), w - 4, pad.top + 8);
  ctx.fillText(min.toFixed(0), w - 4, h - pad.bottom);
}
