#!/usr/bin/env python3
import asyncio
import json
import websockets
from datetime import datetime

async def handler(websocket):
    print(f"Client connected: {websocket.remote_address}")
    try:
        async for message in websocket:
            print(f"Received message: {message}")
            try:
                data = json.loads(message)
                msg_type = data.get("Type")
                
                if msg_type in ["trade_subscribe", "orderbook_subscribe", "funding_subscribe", "mark_price_subscribe"]:
                    sub_data = data.get("Data", {})
                    ticker = sub_data.get("Ticker", "BTC_USDT")
                    print(f"Handling subscription: {msg_type} for ticker: {ticker}")
                    
                    # 1. Send immediate orderbook snapshot to bootstrap the book
                    now_str = datetime.utcnow().isoformat() + "Z"
                    snapshot = {
                        "Type": "orderbook_snapshot",
                        "Data": {
                            "Ticker": ticker,
                            "Asks": [
                                {"Price": 100.01, "Size": 5.0},
                                {"Price": 100.02, "Size": 10.0},
                                {"Price": 100.03, "Size": 15.0}
                            ],
                            "Bids": [
                                {"Price": 100.00, "Size": 5.0},
                                {"Price": 99.99, "Size": 10.0},
                                {"Price": 99.98, "Size": 15.0}
                            ],
                            "Time": now_str
                        }
                    }
                    await websocket.send(json.dumps(snapshot))
                    print(f"Dispatched snapshot for {ticker}")

                    # 2. Spawn a background task to stream updates for this ticker
                    asyncio.create_task(stream_updates(websocket, ticker))
            except Exception as e:
                print(f"Error handling message: {e}")
    except websockets.exceptions.ConnectionClosed as e:
        print(f"Client disconnected: {e}")

async def stream_updates(websocket, ticker):
    try:
        step = 0
        while True:
            await asyncio.sleep(0.5) # updates every 500ms
            step += 1
            now_str = datetime.utcnow().isoformat() + "Z"
            
            # Alternate updates and trade prints
            if step % 2 == 1:
                # Orderbook Update
                # Large density (350.0 lots) on Ask at 100.01 on step 11
                size = 350.0 if step == 11 else 5.0
                update = {
                    "Type": "orderbook_update",
                    "Data": {
                        "Ticker": ticker,
                        "Updates": [
                            {"Price": 100.01, "Size": size, "Type": "Ask"},
                            {"Price": 100.00, "Size": 8.0, "Type": "Bid"}
                        ],
                        "Time": now_str
                    }
                }
                await websocket.send(json.dumps(update))
            else:
                # Trade Update
                trade = {
                    "Type": "trade_update",
                    "Data": {
                        "Ticker": ticker,
                        "Trades": [
                            {"Price": 100.01, "Size": 2.5, "Side": "Buy", "Time": now_str}
                        ]
                    }
                }
                await websocket.send(json.dumps(trade))
                
    except websockets.exceptions.ConnectionClosed:
        pass # Task dies naturally when connection closes

async def main():
    print("Starting Mock MetaScalp Gateway on ws://127.0.0.1:17846")
    async with websockets.serve(handler, "127.0.0.1", 17846):
        await asyncio.Future()  # keep running forever

if __name__ == "__main__":
    asyncio.run(main())
