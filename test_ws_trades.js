const WebSocket = require('ws');

const ws = new WebSocket('ws://127.0.0.1:17845/');

ws.on('open', function open() {
  console.log('> Connected');
  // Subscribe to connection 2 (MEXC Futures)
  ws.send(JSON.stringify({
    Type: 'trade_subscribe',
    Data: { ConnectionId: 2, Ticker: 'BTC_USDT' }
  }));
});

ws.on('message', function incoming(data) {
  const j = JSON.parse(data.toString());
  console.log('< Received:', j.Type, JSON.stringify(j.Data).substring(0, 100));
});

ws.on('error', function error(err) {
  console.error('! Error:', err.message);
});

setTimeout(() => {
  ws.close();
  console.log('> Done');
  process.exit(0);
}, 5000);
