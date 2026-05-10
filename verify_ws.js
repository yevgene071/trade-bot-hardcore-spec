const WebSocket = require('ws');

const ws = new WebSocket('ws://127.0.0.1:17845/ws');

ws.on('open', function open() {
  console.log('> Connected');
  // Subscribe to connection 2 (Futures)
  ws.send(JSON.stringify({
    Type: 'subscribe',
    Data: { ConnectionId: 2 }
  }));
  
  // Subscribe to orderbook for BTC_USDT
  ws.send(JSON.stringify({
    Type: 'orderbook_subscribe',
    Data: { ConnectionId: 2, Ticker: 'BTC_USDT' }
  }));
});

ws.on('message', function incoming(data) {
  const j = JSON.parse(data.toString());
  console.log('< Received:', j.Type, JSON.stringify(j.Data).substring(0, 100));
});

setTimeout(() => {
  ws.close();
  process.exit(0);
}, 5000);
