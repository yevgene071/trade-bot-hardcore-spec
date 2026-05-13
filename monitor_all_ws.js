const WebSocket = require('ws');
const ws = new WebSocket('ws://127.0.0.1:17845/');

ws.on('open', () => {
  console.log('Connected');
  // Subscribing to everything possible to see what pops up
  ws.send(JSON.stringify({ Type: 'notification_subscribe', Data: {} }));
  ws.send(JSON.stringify({ Type: 'signal_level_subscribe', Data: {} }));
  ws.send(JSON.stringify({ Type: 'subscribe', Data: { ConnectionId: 2 } }));
});

ws.on('message', (data) => {
  const msg = JSON.parse(data.toString());
  console.log('--- MESSAGE ---');
  console.log('Type:', msg.Type);
  console.log('Data Keys:', msg.Data ? Object.keys(msg.Data) : 'null');
  if (msg.Type === 'notification_snapshot' || msg.Type === 'notification_update') {
    console.log('Full Notification Data:', JSON.stringify(msg.Data, null, 2));
  }
});

setTimeout(() => {
  console.log('Closing...');
  ws.close();
}, 20000); // 20 seconds
