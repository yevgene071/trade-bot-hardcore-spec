'use strict';

// Core
// @depends-on: core/state.js
// @depends-on: core/formatters.js
// @depends-on: core/dom.js
// @depends-on: core/toast.js
// @depends-on: core/clock.js

// Charts
// @depends-on: charts/canvas.js
// @depends-on: charts/equity.js
// @depends-on: charts/price.js
// @depends-on: charts/mini.js
// @depends-on: charts/zoom.js

// Panels
// @depends-on: panels/header.js
// @depends-on: panels/strategy-perf.js
// @depends-on: panels/funding.js
// @depends-on: panels/risk.js
// @depends-on: panels/command.js
// @depends-on: panels/positions.js
// @depends-on: panels/universe.js
// @depends-on: panels/journal.js
// @depends-on: panels/signals.js
// @depends-on: panels/heatmap.js
// @depends-on: panels/trading.js

// App & Transport
// @depends-on: app/controls.js
// @depends-on: transport/api.js
// @depends-on: app/tabs.js
// @depends-on: app/update.js
// @depends-on: transport/ws.js

/**
 * Bootstrap logic.
 * Phase 8: DOMContentLoaded, connect, start intervals.
 */
document.addEventListener('DOMContentLoaded', () => {
  console.log('Dashboard bootstrap started');

  // Initialize tabs (already called in app/tabs.js, but safe to ensure)
  if (typeof initTabs === 'function') initTabs();

  // Start polling (already called in app/update.js)
  if (typeof poll === 'function') poll();

  // Optional: Connect WebSocket
  if (typeof connectWS === 'function') connectWS();

  // Update clock immediately
  if (typeof updateClock === 'function') updateClock();
});
