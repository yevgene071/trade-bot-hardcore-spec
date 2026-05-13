'use strict';

// @depends-on: ../app/update.js

/**
 * WebSocket transport.
 * Phase 7: Extract connect() + handlers.
 */

function connectWS() {
  const protocol = window.location.protocol === 'https:' ? 'wss:' : 'ws:';
  const ws = new WebSocket(`${protocol}//${window.location.host}/ws`);

  ws.onopen = () => {
    console.log('WS Connected');
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
    setTimeout(connectWS, 2000);
  };

  ws.onerror = (err) => {
    console.error('WS Error:', err);
    ws.close();
  };
}

// WS is optional if polling is active, but we can start it
// connectWS();
