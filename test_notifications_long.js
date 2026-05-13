const WebSocket = require('ws');

const ws = new WebSocket('ws://127.0.0.1:17845/');

ws.on('open', function open() {
  console.log('> Connected');
  ws.send(JSON.stringify({
    Type: 'notification_subscribe',
    Data: {}
  }));
});

ws.on('message', function incoming(data) {
  const s = data.toString();
  console.log('< Received Raw:', s);
  try {
    const j = JSON.parse(s);
    if (j.Type === 'notification_snapshot') {
       console.log('!!! FOUND SNAPSHOT !!!');
    }
  } catch (e) {}
});

setTimeout(() => {
  ws.close();
  process.exit(0);
}, 10000); // 10 seconds
