'use strict';

// @depends-on: dom.js

/**
 * Clock and countdown utilities.
 * Phase 7: Extract updateClock, updateFundingCountdowns.
 */

function updateClock() {
  const el = $('clock');
  if (el) {
    el.textContent = new Date().toISOString().slice(11, 19);
  }
}

function updateFundingCountdowns() {
  document.querySelectorAll('.funding-countdown').forEach(el => {
    const next = parseInt(el.dataset.next);
    if (!next) return;
    const diff = Math.max(0, Math.floor((next - Date.now()) / 1000));
    const h = Math.floor(diff / 3600);
    const m = Math.floor((diff % 3600) / 60);
    const s = diff % 60;
    el.textContent = `${h}:${m.toString().padStart(2, '0')}:${s.toString().padStart(2, '0')}`;
  });
}

// Start clock interval
setInterval(updateClock, 1000);
setInterval(updateFundingCountdowns, 1000);
updateClock();
