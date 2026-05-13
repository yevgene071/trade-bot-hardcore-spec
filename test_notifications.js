const WebSocket = require('ws');

const ws = new WebSocket('ws://127.0.0.1:17845/'); // No /ws suffix according to docs

ws.on('open', function open() {
  console.log('> Connected');
  ws.send(JSON.stringify({
    Type: 'notification_subscribe',
    Data: {}
  }));
});

ws.on('message', function incoming(data) {
  const j = JSON.parse(data.toString());
  console.log('< Received:', j.Type, JSON.stringify(j.Data));
});

ws.on('error', function error(err) {
  console.error('! Error:', err.message);
});

setTimeout(() => {
  ws.close();
  console.log('> Done');
  process.exit(0);
}, 3000);
