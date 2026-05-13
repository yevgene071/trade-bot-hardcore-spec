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
  const stats = data.strategy_perf || [];

  if (!stats.length) {
    body.innerHTML = '<div class="empty-state">No trade data yet</div>';
    return;
  }

  body.replaceChildren();
  stats.forEach(s => {
    const row = el('div', 'strat-perf-row');
    const pnlCls = s.pnl_usd >= 0 ? 'val-up' : 'val-dn';
    row.innerHTML = `
      <div class="strat-info">
        <span class="strat-name">${s.name}</span>
        <span class="strat-meta">${s.trades} trades | WR: ${s.win_rate.toFixed(0)}%</span>
      </div>
      <div class="strat-pnl ${pnlCls}">${fmt$(s.pnl_usd)}</div>
    `;
    body.appendChild(row);
  });
}
