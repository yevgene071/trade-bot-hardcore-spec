'use strict';

// @depends-on: ../core/state.js
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

function updateApp(data) {
  setState(data);

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

// Start polling
setInterval(poll, 1000);
poll();

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
