'use strict';

// @depends-on: ../core/dom.js
// @depends-on: ../core/formatters.js

/**
 * Strategy Performance panel renderer.
 * Phase 4: Extract renderStrategyPerf.
 */
function renderStrategyPerf(data) {
  const body = $('strat-perf-body');
  if (!body) return;
  const stats = data.strategy_stats || [];

  if (!stats.length) {
    body.innerHTML = '<div class="empty-state">No trade data yet</div>';
    return;
  }

  // Hash diffing
  const hash = stats.map((s) => s.name + s.total_trades + s.total_pnl).join('|');
  if (body.dataset.lastHash === hash) return;
  body.dataset.lastHash = hash;

  body.replaceChildren();
  stats.forEach((s) => {
    const row = el('div', 'strat-perf-row');
    const pnlCls = s.total_pnl >= 0 ? 'val-up' : 'val-dn';
    const wr = s.total_trades > 0 ? (s.wins / s.total_trades) * 100 : 0;
    row.innerHTML = `
      <div class="strat-info">
        <span class="strat-name">${s.name}</span>
        <span class="strat-meta">${s.total_trades} trades | WR: ${wr.toFixed(0)}%</span>
      </div>
      <div class="strat-pnl ${pnlCls}">${fmt$(s.total_pnl)}</div>
    `;
    body.appendChild(row);
  });
}
