'use strict';

// @depends-on: ../core/dom.js
// @depends-on: ../core/formatters.js
// @depends-on: ../core/state.js

/**
 * Positions panel renderer.
 * Phase 4: Extract renderPositions.
 */
function renderPositions(data) {
  const body = $('pos-body');
  if (!body) return;
  const rows = data.positions || [];
  const badge = $('pos-tab-badge');
  if (badge) {
    badge.textContent = rows.length;
    badge.style.display = rows.length ? 'inline' : 'none';
  }

  body.replaceChildren();
  if (!rows.length) {
    const tr = el('tr');
    const td = el('td', 'empty-state', 'No active positions');
    td.colSpan = 8;
    tr.appendChild(td);
    body.appendChild(tr);
    return;
  }

  rows.forEach((p) => {
    const tr = el('tr');
    tr.style.cursor = 'pointer';
    tr.addEventListener('click', () => selectTicker(p.ticker));
    if (p.ticker === _selTicker) tr.classList.add('active');

    const sideCls = p.side === 1 ? 'val-up' : 'val-dn';
    const sideTxt = p.side === 1 ? 'LONG' : 'SHORT';

    tr.innerHTML = `
      <td class="mono" style="font-weight:700;">${p.ticker}</td>
      <td class="${sideCls}" style="font-weight:700;">${sideTxt}</td>
      <td class="mono">${fmtT(p.entry_price)}</td>
      <td class="mono">${fmtT(p.mark_price)}</td>
      <td class="mono" style="color:var(--negative-bright)">${fmtT(p.stop_loss)}</td>
      <td class="mono" style="color:var(--positive-bright)">${fmtT(p.take_profit)}</td>
      <td class="mono ${p.pnl_usd >= 0 ? 'val-up' : 'val-dn'}">${fmt$(p.pnl_usd)} (${fmtPct(p.pnl_pct)})</td>
      <td>
        <div class="progress-ring-container">
          <svg class="progress-ring" width="24" height="24">
            <circle class="progress-ring-circle" stroke="var(--muted)" stroke-width="2" fill="transparent" r="10" cx="12" cy="12"/>
            <circle class="progress-ring-circle-fill" stroke="${p.pnl_usd >= 0 ? 'var(--positive-bright)' : 'var(--negative-bright)'}" stroke-width="2" stroke-dasharray="62.83" stroke-dashoffset="${62.83 * (1 - Math.min(1, Math.abs(p.pnl_pct) / 2))}" fill="transparent" r="10" cx="12" cy="12"/>
          </svg>
        </div>
      </td>
    `;
    body.appendChild(tr);
  });
}
