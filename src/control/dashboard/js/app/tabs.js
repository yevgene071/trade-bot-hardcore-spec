'use strict';

// @depends-on: ../core/dom.js
// @depends-on: ../core/state.js
// @depends-on: ../charts/zoom.js
// @depends-on: ../panels/trading.js

/**
 * Tab management.
 * Phase 7: Extract tab switching + forced canvas re-render.
 */

function initTabs() {
  document.querySelectorAll('.tab-btn').forEach((btn) => {
    btn.addEventListener('click', () => {
      const tab = btn.dataset.tab;
      document.querySelectorAll('.tab-btn').forEach((b) => b.classList.remove('active'));
      btn.classList.add('active');
      document.querySelectorAll('.tab-content').forEach((c) => c.classList.remove('active'));

      const content = $('tab-' + tab);
      if (content) content.classList.add('active');

      if (tab === 'trading') {
        setTimeout(() => {
          initChartZoom();
          if (_state) renderTrading(_state, true);
        }, 50);
      }
    });
  });
}

// Initialize tabs on load
initTabs();
