const WebSocket = require('ws');
const ws = new WebSocket('ws://127.0.0.1:17845/');

console.log('Target: MetaScalp WebSocket on 17845');

ws.on('open', () => {
  console.log('> Connected. Sending notification_subscribe...');
  ws.send(JSON.stringify({ Type: 'notification_subscribe', Data: {} }));
});

ws.on('message', (data) => {
  const s = data.toString();
  try {
    const j = JSON.parse(s);
    console.log(`[${new Date().toISOString().slice(11,23)}] < Type: ${j.Type}`);
    
    if (j.Type === 'notification_snapshot' || j.Type === 'notification_update') {
      console.log('--- NOTIFICATION CONTENT ---');
      console.log(JSON.stringify(j.Data, null, 2));
      console.log('----------------------------');
    }
  } catch (e) {
    console.log('< Raw String:', s);
  }
});

ws.on('error', (err) => console.error('! WS Error:', err.message));

setTimeout(() => {
  console.log('> Test finished.');
  ws.close();
  process.exit(0);
}, 30000); // 30 seconds to catch at least something
