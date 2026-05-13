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

  update('equity-val', a.equity_usd, fmt$);
  update('pnl-today', a.realized_pnl_today_usd, fmt$);
  update('free-bal', a.free_balance_usd, fmt$);
  update('unreal-pnl', a.unrealized_pnl_usd, fmt$);

  const pnlToday = $('pnl-today');
  if (pnlToday) pnlToday.className = 'kpi-value xl ' + ((a.realized_pnl_today_usd || 0) >= 0 ? 'val-up' : 'val-dn');

  const unreal = $('unreal-pnl');
  if (unreal) unreal.className = 'kpi-value lg ' + ((a.unrealized_pnl_usd || 0) >= 0 ? 'val-up' : 'val-dn');

  // Compute metrics from strategy_stats + risk (server has no dedicated metrics field)
  const stats = data.strategy_stats || [];
  let totalWins = 0, totalLosses = 0, totalTrades = 0, totalGrossProfit = 0, totalGrossLoss = 0;
  stats.forEach((s) => {
    totalWins += s.wins || 0;
    totalLosses += s.losses || 0;
    totalTrades += s.total_trades || 0;
    totalGrossProfit += s.gross_profit || 0;
    totalGrossLoss += s.gross_loss || 0;
  });
  const m = {
    win_rate: (totalWins + totalLosses) > 0 ? (totalWins / (totalWins + totalLosses)) * 100 : 0,
    profit_factor: totalGrossLoss > 0 ? totalGrossProfit / totalGrossLoss : (totalGrossProfit > 0 ? 99.9 : 0),
    total_trades: totalTrades,
    max_drawdown: (data.risk && data.risk.current_drawdown_pct) || 0,
  };
  update('m-wr', m.win_rate, (v) => (v || 0).toFixed(0) + '%');
  update('m-pf', m.profit_factor, (v) => (v || 0).toFixed(2));
  update('m-trades', m.total_trades, (v) => v || 0);
  update('max-dd', m.max_drawdown, (v) => (v || 0).toFixed(1) + '%');
}
