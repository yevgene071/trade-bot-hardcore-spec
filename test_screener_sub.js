const WebSocket = require('ws');
const ws = new WebSocket('ws://127.0.0.1:17845/');

ws.on('open', () => {
  console.log('Connected');
  ws.send(JSON.stringify({ Type: 'screener_subscribe', Data: {} }));
});

ws.on('message', (data) => {
  console.log('< Received:', data.toString());
});

setTimeout(() => {
  ws.close();
  process.exit(0);
}, 3000);
