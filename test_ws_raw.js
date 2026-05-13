const WebSocket = require('ws');

const ws = new WebSocket('ws://127.0.0.1:17845/');

ws.on('open', function open() {
  console.log('> Connected');
  ws.send(JSON.stringify({
    Type: 'orderbook_subscribe',
    Data: { ConnectionId: 2, Ticker: 'BTC_USDT' }
  }));
});

ws.on('message', function incoming(data) {
  console.log('< Raw:', data.toString());
});

setTimeout(() => {
  ws.close();
  process.exit(0);
}, 2000);
