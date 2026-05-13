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
  console.log('< Raw:', s);
  const j = JSON.parse(s);
  if (j.Type === 'notification_snapshot' || j.Type === 'notification_update') {
    if (j.Data) {
       console.log('Data keys:', Object.keys(j.Data));
    }
  }
});

setTimeout(() => {
  ws.close();
  process.exit(0);
}, 5000);
