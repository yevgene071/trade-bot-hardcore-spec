'use strict';

// @depends-on: ../core/dom.js
// @depends-on: ../core/formatters.js

/**
 * Risk Dashboard panel renderer.
 * Phase 4: Extract renderRisk and renderGauge.
 */
function renderRisk(data) {
  const r = data.risk || {};

  // Gauges
  renderGauge('margin-gauge', r.margin_used_pct || 0, '%');
  renderGauge('exposure-gauge', r.exposure_pct || 0, '%', 100);
  renderGauge('daily-pnl-gauge', r.daily_pnl_pct || 0, '%', 2); // 2% limit

  // Progress bars
  const lossFill = $('loss-limit-bar');
  if (lossFill) {
    const dailyPnlUsd = (data.account && data.account.realized_pnl_today_usd) || 0;
    const dailyLossLimit = Math.abs(r.daily_pnl_pct || 0) > 0.001 ? Math.abs(dailyPnlUsd / (r.daily_pnl_pct / 100)) * 2 : 1000; // rough derived limit
    const pct = Math.min(100, (Math.abs(dailyPnlUsd) / Math.max(1, dailyLossLimit)) * 100);
    lossFill.style.width = pct + '%';
    lossFill.style.background = pct > 80 ? 'var(--negative-bright)' : 'var(--accent)';
  }

  const slotsFill = $('pos-slots-bar');
  const slotsLabel = slotsFill ? slotsFill.parentElement.previousElementSibling : null;
  if (slotsFill) {
    const openTrades = (data.open_trades && data.open_trades.length) || 0;
    const total = 3;
    const pct = (openTrades / total) * 100;
    slotsFill.style.width = pct + '%';
    if (slotsLabel) slotsLabel.textContent = `Position Slots (${openTrades}/${total})`;
  }
}

function renderGauge(id, val, unit, max = 100) {
  const el = $(id);
  if (!el) return;
  const pct = Math.min(100, (val / max) * 100);
  const color =
    pct > 80 ? 'var(--negative-bright)' : pct > 50 ? 'var(--warning)' : 'var(--positive-bright)';

  el.style.background = `conic-gradient(${color} ${pct}%, var(--bg-card) 0)`;
  el.innerHTML = `<div class="gauge-inner"><div class="gauge-val">${unit === '$' ? fmtSz(val) : val.toFixed(1) + unit}</div></div>`;
}
