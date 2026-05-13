const WebSocket = require('ws');

const ws = new WebSocket('ws://127.0.0.1:17845/');

ws.on('open', function open() {
  console.log('> Connected');
  ws.send(JSON.stringify({
    Type: 'signal_level_subscribe',
    Data: {}
  }));
});

ws.on('message', function incoming(data) {
  console.log('< Received:', data.toString());
});

setTimeout(() => {
  ws.close();
  process.exit(0);
}, 3000);
