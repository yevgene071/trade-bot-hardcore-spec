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
  const rows = data.open_trades || [];
  const badge = $('pos-tab-badge');
  if (badge) {
    badge.textContent = rows.length;
    badge.style.display = rows.length ? 'inline' : 'none';
  }

  // Hash-based diffing to avoid DOM thrashing
  const hash = rows.map((p) => p.plan.ticker + p.plan.side + p.avg_entry_price + p.plan.stop_price + p.plan.tp1_price).join('|');
  if (body.dataset.lastHash === hash) {
    // Fast path: update mark and PnL cells
    const trs = body.querySelectorAll('tr');
    rows.forEach((p, i) => {
      if (i >= trs.length) return;
      const tr = trs[i];
      const side = p.plan.side || 0;
      const pnlUsd = p.unrealized_pnl || 0;
      const entry = p.avg_entry_price || 0;
      const mark = p.plan.ticker === _selTicker ? (data.ob_mid || entry) : entry;
      const pnlPct = entry > 0 ? ((mark - entry) / entry) * (side === 1 ? 100 : -100) : 0;
      
      const tds = tr.children;
      if (tds.length >= 8) {
        tds[3].textContent = fmtT(mark);
        tds[6].className = `mono ${pnlUsd >= 0 ? 'val-up' : 'val-dn'}`;
        tds[6].textContent = `${fmt$(pnlUsd)} (${fmtPct(pnlPct)})`;
        const circle = tr.querySelector('.progress-ring-circle-fill');
        if (circle) {
          circle.setAttribute('stroke', pnlUsd >= 0 ? 'var(--positive-bright)' : 'var(--negative-bright)');
          circle.setAttribute('stroke-dashoffset', 62.83 * (1 - Math.min(1, Math.abs(pnlPct) / 2)));
        }
      }
    });
    return;
  }
  body.dataset.lastHash = hash;

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
    tr.addEventListener('click', () => selectTicker(p.plan.ticker));
    if (p.plan.ticker === _selTicker) tr.classList.add('active');

    const side = p.plan.side || 0;
    const sideCls = side === 1 ? 'val-up' : 'val-dn';
    const sideTxt = side === 1 ? 'LONG' : 'SHORT';
    const pnlUsd = p.unrealized_pnl || 0;
    const entry = p.avg_entry_price || 0;
    const mark = p.plan.ticker === _selTicker ? (data.ob_mid || entry) : entry;
    const pnlPct = entry > 0 ? ((mark - entry) / entry) * (side === 1 ? 100 : -100) : 0;

    tr.innerHTML = `
      <td class="mono" style="font-weight:700;">${p.plan.ticker}</td>
      <td class="${sideCls}" style="font-weight:700;">${sideTxt}</td>
      <td class="mono">${fmtT(entry)}</td>
      <td class="mono">${fmtT(mark)}</td>
      <td class="mono" style="color:var(--negative-bright)">${fmtT(p.plan.stop_price)}</td>
      <td class="mono" style="color:var(--positive-bright)">${fmtT(p.plan.tp1_price)}</td>
      <td class="mono ${pnlUsd >= 0 ? 'val-up' : 'val-dn'}">${fmt$(pnlUsd)} (${fmtPct(pnlPct)})</td>
      <td>
        <div class="progress-ring-container">
          <svg class="progress-ring" width="24" height="24">
            <circle class="progress-ring-circle" stroke="var(--muted)" stroke-width="2" fill="transparent" r="10" cx="12" cy="12"/>
            <circle class="progress-ring-circle-fill" stroke="${pnlUsd >= 0 ? 'var(--positive-bright)' : 'var(--negative-bright)'}" stroke-width="2" stroke-dasharray="62.83" stroke-dashoffset="${62.83 * (1 - Math.min(1, Math.abs(pnlPct) / 2))}" fill="transparent" r="10" cx="12" cy="12"/>
          </svg>
        </div>
      </td>
    `;
    body.appendChild(tr);
  });
}
