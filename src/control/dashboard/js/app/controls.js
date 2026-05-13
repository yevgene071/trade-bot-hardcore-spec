'use strict';

// @depends-on: ../core/dom.js

/**
 * Controls Panel renderer.
 * Phase 7: Extract renderControls.
 */

let _logPollerStarted = false;

function _startLogPoller() {
  if (_logPollerStarted) return;
  _logPollerStarted = true;
  function fetchLogs() {
    fetch('/api/logs?n=80')
      .then((r) => r.json())
      .then((lines) => {
        const feed = $('log-feed');
        if (!feed) return;
        feed.replaceChildren();
        if (!lines.length) {
          feed.appendChild(el('div', 'empty-state', 'No logs'));
          return;
        }
        lines
          .slice()
          .reverse()
          .forEach((line) => {
            const div = document.createElement('div');
            div.style.cssText =
              'font-family:var(--mono);font-size:10px;padding:1px 0;border-bottom:1px solid rgba(255,255,255,0.04);white-space:pre-wrap;word-break:break-all;';
            div.textContent = line;
            feed.appendChild(div);
          });
        feed.scrollTop = feed.scrollHeight;
      })
      .catch(() => {});
  }
  fetchLogs();
  setInterval(fetchLogs, 5000);
}

function renderControls(data) {
  const ks = $('ks-banner');
  if (ks) {
    ks.style.display = data.kill_switch_active ? 'block' : 'none';
  }

  const ksBtn = $('ks-btn');
  if (ksBtn) {
    ksBtn.textContent = data.kill_switch_active ? 'DEACTIVATE KILL-SWITCH' : 'ACTIVATE KILL-SWITCH';
    ksBtn.className = 'btn ' + (data.kill_switch_active ? 'btn-pos' : 'btn-neg');
    // Ensure only one click listener
    if (!ksBtn.dataset.ksInit) {
      ksBtn.dataset.ksInit = '1';
      ksBtn.addEventListener('click', () => {
        fetch('/api/killswitch/toggle', { method: 'POST' })
          .then((r) => r.json())
          .then((j) => {
            if (j.ok) {
              toast('Kill Switch', j.active ? 'Activated' : 'Deactivated', j.active ? 'neg' : 'pos');
            } else {
              toast('Kill Switch Error', j.error || 'Failed', 'neg');
            }
          })
          .catch(() => toast('Kill Switch Error', 'Network error', 'neg'));
      });
    }
  }

  // Connections panel
  const ms = data.metascalp || {};
  const msConn = $('cfg-ms-conn');
  if (msConn) msConn.textContent = ms.connected ? 'Connected' : 'Disconnected';
  const msLat = $('cfg-ms-lat');
  if (msLat) msLat.textContent = ms.connected ? (ms.latency_ms || 0) + ' ms' : '—';
  const modeEl = $('cfg-mode');
  if (modeEl) modeEl.textContent = data.kill_switch_active ? 'KILL-SWITCH' : 'Live';
  const versionEl = $('cfg-version');
  if (versionEl) versionEl.textContent = data.version || '—';

  // Start log poller once
  _startLogPoller();
}
