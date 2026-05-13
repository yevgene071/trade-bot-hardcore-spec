'use strict';

// @depends-on: ../core/dom.js

/**
 * Controls Panel renderer.
 * Phase 7: Extract renderControls.
 */

function renderControls(data) {
  const ks = $('kill-switch-banner');
  if (ks) {
    ks.style.display = data.kill_switch_active ? 'block' : 'none';
  }

  const ksBtn = $('ks-btn');
  if (ksBtn) {
    ksBtn.textContent = data.kill_switch_active ? 'DEACTIVATE KILL-SWITCH' : 'ACTIVATE KILL-SWITCH';
    ksBtn.className = 'btn ' + (data.kill_switch_active ? 'btn-pos' : 'btn-neg');
  }
}
