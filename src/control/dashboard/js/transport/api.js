'use strict';

// @depends-on: ../core/state.js
// @depends-on: ../core/toast.js
// @depends-on: ../panels/trading.js
// @depends-on: ../charts/zoom.js

/**
 * API and Network layer.
 * Phase 7: Extract selectTicker, sendCommand, startRecorder, stopRecorder, exportCSV.
 */

function selectTicker(t) {
  setSelTicker(t);
  setSelTickerTime(Date.now());
  setChartZoom(null);
  updateZoomIndicator();
  fetch('/api/ticker/select', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ ticker: t })
  }).then(r => r.json()).then(j => {
    if (j.ok && _state) renderTrading(_state, true);
  });
}

function sendCommand(cmd, args = {}) {
  fetch('/api/command', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ command: cmd, args })
  }).then(r => r.json()).then(j => {
    if (j.ok) toast('Command Sent', cmd + ' ok', 'pos');
    else toast('Command Failed', j.error || 'Error', 'neg');
  }).catch(() => toast('Command Error', 'Network error', 'neg'));
}

function startRecorder() {
  const el = $('rec-filename');
  const fn = (el ? el.value || 'dump' : 'dump').replace(/[^a-zA-Z0-9\-_.]/g, '');
  fetch('/api/dump/start', { 
    method: 'POST', 
    headers: { 'Content-Type': 'application/json' }, 
    body: JSON.stringify({ filename: fn }) 
  }).then(r => r.json()).then(j => { 
    if (j.ok) { 
      const status = $('rec-status');
      if (status) {
        status.textContent = '● RECORDING'; 
        status.className = 'val-dn'; 
      }
      const startBtn = $('rec-start-btn');
      if (startBtn) startBtn.disabled = true; 
      const stopBtn = $('rec-stop-btn');
      if (stopBtn) stopBtn.disabled = false; 
    } else {
      toast('Recorder Error', j.error || 'Failed', 'neg'); 
    }
  }).catch(() => toast('Recorder Error', 'Network error', 'neg'));
}

function stopRecorder() {
  fetch('/api/dump/stop', { method: 'POST' }).then(r => r.json()).then(() => { 
    const status = $('rec-status');
    if (status) {
      status.textContent = 'Idle'; 
      status.className = ''; 
    }
    const startBtn = $('rec-start-btn');
    if (startBtn) startBtn.disabled = false; 
    const stopBtn = $('rec-stop-btn');
    if (stopBtn) stopBtn.disabled = true; 
  }).catch(() => { });
}

function exportCSV() {
  if (!_state || !_state.recent_journal || !_state.recent_journal.length) return;
  let csv = 'Ticker,Side,Strategy,Entry Price,Exit Price,PnL USD,Exit Reason\n';
  _state.recent_journal.forEach(e => { 
    csv += `${e.plan.ticker},${e.plan.side === 1 ? 'LONG' : 'SHORT'},${e.plan.strategy_name},${e.plan.entry_price},${e.exit_price},${e.pnl_usd},${e.cause_of_exit || ''}\n`; 
  });
  const blob = new Blob([csv], { type: 'text/csv' });
  const url = URL.createObjectURL(blob);
  const a = document.createElement('a');
  a.href = url;
  a.download = `journal_${new Date().toISOString().slice(0, 10)}.csv`;
  a.click();
}
