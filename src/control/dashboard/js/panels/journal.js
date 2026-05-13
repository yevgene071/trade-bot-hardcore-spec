'use strict';

// @depends-on: ../core/dom.js
// @depends-on: ../core/formatters.js
// @depends-on: ../core/state.js

/**
 * Journal panel renderer.
 * Phase 4: Extract renderJournal.
 */
function renderJournal(data) {
  const body = $('jrn-body');
  if (!body) return;
  const allEntries = (data.recent_journal || []).slice(0, 50);
  const stratFilter = $('jrn-strat-filter');
  const curStrat = stratFilter ? stratFilter.value : '';
  const entries = curStrat ? allEntries.filter(e => e.plan.strategy_name.includes(curStrat)) : allEntries;
  
  const countEl = $('jrn-count');
  if (countEl) countEl.textContent = entries.length;

  body.replaceChildren();
  if (!entries.length) {
    const tr = el('tr');
    const td = el('td', 'empty-state', 'No journal entries');
    td.colSpan = 7;
    tr.appendChild(td);
    body.appendChild(tr);
    return;
  }

  entries.forEach(e => {
    const tr = el('tr');
    const pnlCls = e.pnl_usd >= 0 ? 'val-up' : 'val-dn';
    tr.innerHTML = `
      <td class="time">${new Date(e.exit_ts).toISOString().slice(11, 19)}</td>
      <td class="mono" style="font-weight:700;">${e.plan.ticker}</td>
      <td class="${e.plan.side === 1 ? 'val-up' : 'val-dn'}">${e.plan.side === 1 ? 'LONG' : 'SHORT'}</td>
      <td>${stratBadge(e.plan.strategy_name)}</td>
      <td class="mono ${pnlCls}">${fmt$(e.pnl_usd)}</td>
      <td class="mono">${fmtT(e.exit_price)}</td>
      <td class="muted" style="font-size:10px;">${e.cause_of_exit || '—'}</td>
    `;
    body.appendChild(tr);
  });
}
