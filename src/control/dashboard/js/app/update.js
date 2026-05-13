'use strict';

// @depends-on: ../core/state.js
// @depends-on: ../core/toast.js
// @depends-on: ../panels/header.js
// @depends-on: ../panels/command.js
// @depends-on: ../panels/risk.js
// @depends-on: ../panels/strategy-perf.js
// @depends-on: ../panels/funding.js
// @depends-on: ../panels/heatmap.js
// @depends-on: ../panels/trading.js
// @depends-on: ../panels/positions.js
// @depends-on: ../panels/signals.js
// @depends-on: ../panels/universe.js
// @depends-on: ../panels/journal.js
// @depends-on: ../charts/equity.js
// @depends-on: controls.js

/**
 * Update dispatcher.
 * Phase 7: Extract update(data) / poll().
 */

// Notification state tracking
let _prevOpenTickers = new Set();
let _prevJournalKeys = new Set();
let _prevKillSwitch = null;
let _prevMsConnected = null;

function _notifyEvents(data) {
  // Kill-switch toggled
  if (_prevKillSwitch !== null && data.kill_switch_active !== _prevKillSwitch) {
    toast(
      'Kill Switch',
      data.kill_switch_active ? 'Activated' : 'Deactivated',
      data.kill_switch_active ? 'neg' : 'pos'
    );
  }
  _prevKillSwitch = data.kill_switch_active;

  // MetaScalp connection changed
  const msConnected = data.metascalp && data.metascalp.connected;
  if (_prevMsConnected !== null && msConnected !== _prevMsConnected) {
    toast('MetaScalp', msConnected ? 'Connected' : 'Disconnected', msConnected ? 'pos' : 'neg');
  }
  _prevMsConnected = !!msConnected;

  // New open trades
  const openTrades = data.open_trades || [];
  const curOpenTickers = new Set(openTrades.map((t) => t.plan.ticker + t.plan.strategy_name));
  curOpenTickers.forEach((key) => {
    if (!_prevOpenTickers.has(key)) {
      const t = openTrades.find((x) => x.plan.ticker + x.plan.strategy_name === key);
      if (t) {
        toast(
          'Trade Opened',
          `${t.plan.ticker} ${t.plan.side === 1 ? 'LONG' : 'SHORT'} @ ${(t.avg_entry_price || t.plan.entry_price || 0).toFixed(2)}`,
          t.plan.side === 1 ? 'pos' : 'neg'
        );
      }
    }
  });
  _prevOpenTickers = curOpenTickers;

  // Closed trades (appeared in journal)
  const journal = data.recent_journal || [];
  journal.forEach((e) => {
    const key = e.ts_unix_ms + e.plan.ticker;
    if (!_prevJournalKeys.has(key)) {
      _prevJournalKeys.add(key);
      const pnl = e.pnl_usd || 0;
      toast(
        'Trade Closed',
        `${e.plan.ticker} PnL: ${pnl >= 0 ? '+' : ''}$${pnl.toFixed(2)} (${e.cause_of_exit || '—'})`,
        pnl >= 0 ? 'pos' : 'neg'
      );
    }
  });
  if (_prevJournalKeys.size > 200) {
    _prevJournalKeys = new Set([..._prevJournalKeys].slice(-100));
  }
}

function updateApp(data) {
  setState(data);
  _notifyEvents(data);

  // Panels
  renderHeader(data);
  renderCommand(data);
  renderRisk(data);
  renderStrategyPerf(data);
  renderFunding(data);

  renderHeatmap(data);
  renderDetail(data);
  renderConditions(data);
  renderTrading(data);
  renderPositions(data);
  renderSignals(data);
  renderUniverse(data);
  renderJournal(data);

  // Charts
  renderEquityChart(data);

  // Controls
  renderControls(data);
}

function poll() {
  fetch('/api/state')
    .then((r) => r.json())
    .then((data) => {
      updateApp(data);
    })
    .catch((err) => {
      console.error('Poll error:', err);
    });
}

// poll interval started by main.js bootstrap inside DOMContentLoaded

// Handle resize
window.addEventListener('resize', () => {
  if (typeof _resizeTimer !== 'undefined') {
    clearTimeout(_resizeTimer);
    setResizeTimer(
      setTimeout(() => {
        if (_state) renderTrading(_state, true);
      }, 100)
    );
  }
});
