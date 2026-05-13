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
    const pnl = data.pnl_today || 0;
    pnlBadge.textContent = fmt$(pnl);
    pnlBadge.className = 'pnl-badge ' + (pnl >= 0 ? 'up' : 'dn');
  }

  // Uptime
  const uptime = $('uptime');
  if (uptime) {
    const sec = g.uptime_sec || 0;
    const h = Math.floor(sec / 3600);
    const m = Math.floor((sec % 3600) / 60);
    const s = sec % 60;
    uptime.textContent = `${h.toString().padStart(2, '0')}:${m.toString().padStart(2, '0')}:${s.toString().padStart(2, '0')}`;
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
    const ok = data.ms_connected;
    msDot.className = 'dot' + (ok ? ' ok' : '');
    msLabel.textContent = ok ? 'ON' : 'OFF';
  }

  // WebSocket status
  const wsDot = $('ws-dot');
  const wsLabel = $('ws-label');
  if (wsDot && wsLabel) {
    const ok = data.ws_connected;
    wsDot.className = 'dot' + (ok ? ' ok' : '');
    wsLabel.textContent = ok ? 'ON' : 'OFF';
  }

  // Clock
  const clock = $('clock');
  if (clock) {
    clock.textContent = new Date().toISOString().slice(11, 19);
  }
}
