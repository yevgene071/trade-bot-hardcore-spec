'use strict';

// @depends-on: canvas.js

/**
 * Price chart renderer.
 * Phase 5: Extract drawPriceChart (formerly drawTradeChart).
 */
function drawPriceChart(ctx, w, h, history) {
  if (history.length < 2) return;
  const pad = { top: 10, right: 45, bottom: 20, left: 10 }, pw = w - pad.left - pad.right, ph = h - pad.top - pad.bottom;

  const mids = history.map(p => p.mid);
  const min = Math.min(...mids), max = Math.max(...mids), range = (max - min) || 1;
  const toX = i => pad.left + (i / (mids.length - 1)) * pw;
  const toY = v => pad.top + ph - ((v - min) / range) * ph;

  drawGrid(ctx, w, h, pad);

  // Volatility band
  ctx.fillStyle = 'rgba(99,102,241,0.06)';
  ctx.beginPath(); 
  ctx.moveTo(toX(0), toY(mids[0] * (1 + 0.002)));
  history.forEach((p, i) => ctx.lineTo(toX(i), toY(p.mid * (1 + p.volatility_1min_bps / 10000))));
  history.slice().reverse().forEach((p, i) => ctx.lineTo(toX(mids.length - 1 - i), toY(p.mid * (1 - p.volatility_1min_bps / 10000))));
  ctx.closePath(); 
  ctx.fill();

  // Mid line
  ctx.beginPath(); 
  ctx.moveTo(toX(0), toY(mids[0]));
  mids.forEach((v, i) => ctx.lineTo(toX(i), toY(v)));
  ctx.strokeStyle = '#e8edf5'; 
  ctx.lineWidth = 1.2; 
  ctx.stroke();

  // Volume bars
  const maxVol = Math.max(...history.map(p => p.buy_vol_5s + p.sell_vol_5s), 1);
  const barW = Math.max(1, pw / history.length * 0.7);
  history.forEach((p, i) => {
    const x = toX(i) - barW / 2;
    const buyH = (p.buy_vol_5s / maxVol) * (ph * 0.25);
    const sellH = (p.sell_vol_5s / maxVol) * (ph * 0.25);
    if (buyH > 0) { ctx.fillStyle = 'rgba(16,185,129,0.4)'; ctx.fillRect(x, pad.top + ph - buyH, barW, buyH); }
    if (sellH > 0) { ctx.fillStyle = 'rgba(239,68,68,0.4)'; ctx.fillRect(x, pad.top + ph - buyH - sellH, barW, sellH); }
  });

  // Y labels
  ctx.fillStyle = '#7b859c'; 
  ctx.font = '8px Inter, sans-serif'; 
  ctx.textAlign = 'right';
  for (let i = 0; i <= 4; i++) { 
    const v = min + (range * i / 4); 
    ctx.fillText(v.toFixed(1), w - 4, toY(v) + 3); 
  }

  // X labels (time)
  ctx.textAlign = 'center';
  const step = Math.max(1, Math.floor(history.length / 6));
  for (let i = 0; i < history.length; i += step) {
    const d = new Date(history[i].ts); 
    ctx.fillText(d.toISOString().slice(11, 19), toX(i), h - 4);
  }
}
