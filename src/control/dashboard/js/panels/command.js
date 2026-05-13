'use strict';

// @depends-on: ../core/dom.js
// @depends-on: ../core/formatters.js

/**
 * Command tab (Account KPIs) panel renderer.
 * Phase 4: Extract renderCommand.
 */
function renderCommand(data) {
  const a = data.account || {};

  const update = (id, val, fmt) => {
    const el = $(id);
    if (el) el.textContent = fmt(val);
  };

  update('equity-val', a.equity, fmt$);
  update('pnl-today', a.pnl_today, fmt$);
  update('free-bal', a.free_balance, fmt$);
  update('unreal-pnl', a.unrealized_pnl, fmt$);

  const pnlToday = $('pnl-today');
  if (pnlToday) pnlToday.className = 'kpi-value xl ' + (a.pnl_today >= 0 ? 'val-up' : 'val-dn');

  const unreal = $('unreal-pnl');
  if (unreal) unreal.className = 'kpi-value lg ' + (a.unrealized_pnl >= 0 ? 'val-up' : 'val-dn');

  // Metrics
  const m = data.metrics || {};
  update('m-wr', m.win_rate, (v) => (v || 0).toFixed(0) + '%');
  update('m-pf', m.profit_factor, (v) => (v || 0).toFixed(2));
  update('m-trades', m.total_trades, (v) => v || 0);
  update('max-dd', m.max_drawdown, (v) => (v || 0).toFixed(1) + '%');
}
