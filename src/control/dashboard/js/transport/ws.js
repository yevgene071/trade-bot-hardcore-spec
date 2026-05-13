'use strict';

// @depends-on: ../core/toast.js
// @depends-on: ../app/update.js

/**
 * WebSocket transport.
 * Phase 7: Extract connect() + handlers.
 */

let _wsWasConnected = false;

function connectWS() {
  const protocol = window.location.protocol === 'https:' ? 'wss:' : 'ws:';
  const ws = new WebSocket(`${protocol}//${window.location.host}/ws`);

  ws.onopen = () => {
    console.log('WS Connected');
    window.__wsConnected = true;
    if (_wsWasConnected) {
      toast('WebSocket', 'Reconnected', 'pos');
    }
    _wsWasConnected = true;
  };

  ws.onmessage = (e) => {
    try {
      const data = JSON.parse(e.data);
      updateApp(data);
    } catch (err) {
      console.error('WS Message error:', err);
    }
  };

  ws.onclose = () => {
    console.log('WS Closed, reconnecting...');
    window.__wsConnected = false;
    if (_wsWasConnected) {
      toast('WebSocket', 'Disconnected — reconnecting…', 'neg');
    }
    setTimeout(connectWS, 2000);
  };

  ws.onerror = (err) => {
    console.error('WS Error:', err);
    ws.close();
  };
}

// WS is optional if polling is active, but we can start it
// connectWS();
