'use strict';

// @depends-on: ../core/dom.js
// @depends-on: ../core/formatters.js

/**
 * Header panel renderer.
 * Phase 4: Extract renderHeader.
 */
function renderHeader(data) {
  const g = data.gauges || {};

  // Brand version
  const vTag = $('v-tag');
  if (vTag) vTag.textContent = data.version || 'v0.0.1';

  // PnL Badge
  const pnlBadge = $('pnl-badge');
  if (pnlBadge) {
    const pnl = (data.account && data.account.realized_pnl_today_usd) || 0;
    pnlBadge.textContent = fmt$(pnl);
    pnlBadge.className = 'pnl-badge ' + (pnl >= 0 ? 'up' : 'dn');
  }

  // Uptime (session uptime — time since dashboard loaded)
  const uptime = $('uptime');
  if (uptime) {
    const secs = Math.floor((Date.now() - _startTime) / 1000);
    const h = Math.floor(secs / 3600).toString().padStart(2, '0');
    const m = Math.floor((secs % 3600) / 60).toString().padStart(2, '0');
    const s = (secs % 60).toString().padStart(2, '0');
    uptime.textContent = `${h}:${m}:${s}`;
  }

  // Recorder status
  const recDot = $('rec-dot');
  const recLabel = $('rec-label');
  if (recDot && recLabel) {
    const active = data.recorder_active;
    recDot.className = 'rec-dot' + (active ? ' active' : '');
    recLabel.textContent = active ? 'RECORDING' : 'REC';
  }

  // MetaScalp status
  const msDot = $('ms-dot');
  const msLabel = $('ms-label');
  if (msDot && msLabel) {
    const ok = data.metascalp && data.metascalp.connected;
    msDot.className = 'dot' + (ok ? ' ok' : '');
    msLabel.textContent = ok ? 'ON' : 'OFF';
  }

  // WebSocket status (local — set by transport/ws.js)
  const wsDot = $('ws-dot');
  const wsLabel = $('ws-label');
  if (wsDot && wsLabel) {
    const ok = window.__wsConnected || false;
    wsDot.className = 'dot' + (ok ? ' ok' : '');
    wsLabel.textContent = ok ? 'ON' : 'OFF';
  }

  // Clock
  const clock = $('clock');
  if (clock) {
    clock.textContent = new Date().toISOString().slice(11, 19);
  }
}
