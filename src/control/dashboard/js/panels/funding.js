'use strict';

// @depends-on: ../core/dom.js
// @depends-on: ../core/formatters.js

/**
 * Funding Rates panel renderer.
 * Phase 4: Extract renderFunding.
 */
function renderFunding(data) {
  const body = $('funding-tbody');
  if (!body) return;
  const rates = data.funding_rates || [];

  if (!rates.length) {
    body.innerHTML = '<tr><td colspan="3" class="empty-state">No funding data</td></tr>';
    return;
  }

  body.replaceChildren();
  rates.forEach((r) => {
    const tr = el('tr');
    const rateCls = r.rate >= 0 ? 'val-up' : 'val-dn';
    tr.innerHTML = `
      <td class="mono">${r.ticker}</td>
      <td class="mono ${rateCls}">${(r.rate * 100).toFixed(4)}%</td>
      <td class="mono muted">${r.next_ts_str || '—'}</td>
    `;
    body.appendChild(tr);
  });
}
