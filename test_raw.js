const WebSocket = require('ws');
const ws = new WebSocket('ws://127.0.0.1:17845/ws');

ws.on('open', () => {
    console.log('CONNECTED');
    ws.send(JSON.stringify({ Type: 'subscribe', Data: { ConnectionId: 2 } }));
    ws.send(JSON.stringify({ Type: 'orderbook_subscribe', Data: { ConnectionId: 2, Ticker: 'BTC_USDT' } }));
});

ws.on('message', (data) => {
    console.log('RAW:', data.toString());
    process.exit(0); // Берем только первое сообщение и выходим
});

setTimeout(() => process.exit(1), 5000);
