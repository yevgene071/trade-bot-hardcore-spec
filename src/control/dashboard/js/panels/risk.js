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
  renderGauge('exposure-gauge', r.exposure_usd || 0, '$', r.exposure_max_usd);
  renderGauge('daily-pnl-gauge', r.daily_pnl_pct || 0, '%', 2); // 2% limit

  // Progress bars
  const lossFill = $('loss-limit-bar');
  if (lossFill) {
    const pct = Math.min(100, (Math.abs(r.daily_pnl_usd || 0) / (r.daily_loss_limit || 1)) * 100);
    lossFill.style.width = pct + '%';
    lossFill.style.background = pct > 80 ? 'var(--negative-bright)' : 'var(--accent)';
  }

  const slotsFill = $('pos-slots-bar');
  const slotsLabel = slotsFill ? slotsFill.parentElement.previousElementSibling : null;
  if (slotsFill) {
    const used = r.pos_slots_used || 0;
    const total = r.pos_slots_total || 3;
    const pct = (used / total) * 100;
    slotsFill.style.width = pct + '%';
    if (slotsLabel) slotsLabel.textContent = `Position Slots (${used}/${total})`;
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
