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
  const rates = data.funding_info || [];

  if (!rates.length) {
    body.innerHTML = '<tr><td colspan="3" class="empty-state">No funding data</td></tr>';
    return;
  }

  // Hash diffing
  const hash = rates.map((r) => r.ticker + r.rate + r.next_funding_unix).join('|');
  if (body.dataset.lastHash === hash) return;
  body.dataset.lastHash = hash;

  body.replaceChildren();
  rates.forEach((r) => {
    const tr = el('tr');
    const rateCls = r.rate >= 0 ? 'val-up' : 'val-dn';
    const nextTs = r.next_funding_unix ? new Date(r.next_funding_unix * 1000).toISOString().slice(11, 19) : '—';
    tr.innerHTML = `
      <td class="mono">${r.ticker}</td>
      <td class="mono ${rateCls}">${(r.rate * 100).toFixed(4)}%</td>
      <td class="mono muted">${nextTs}</td>
    `;
    body.appendChild(tr);
  });
}
