#!/bin/bash
# Script to run trade_bot and record market data dump.

set -e

DURATION_SEC=36000 # 10 hours default
DUMP_NAME="mexc_futures_zec_btc_$(date +%Y%m%d_%H%M%S)"
PORT=8080

# Parse arguments
while [[ $# -gt 0 ]]; do
    case $1 in
        --duration)
            DURATION_SEC="$2"
            shift 2
            ;;
        --name)
            DUMP_NAME="$2"
            shift 2
            ;;
        *)
            echo "Unknown option: $1"
            echo "Usage: $0 [--duration <seconds>] [--name <dump_name>]"
            exit 1
            ;;
    esac
done

echo "=== 1. Checking release binary ==="
if [ ! -f "./build/release/bin/trade_bot" ]; then
    echo "Error: ./build/release/bin/trade_bot not found."
    echo "Please build the release binary first by running:"
    echo "  ./scripts/build.sh release --bin"
    exit 1
fi

echo "=== WARNING ==="
echo "Make sure your config.toml under [trading] symbols is configured with"
echo "all target tickers you want to record (e.g., [\"ZEC_USDT\"])!"
echo "If tickers are missing from config.toml, they will not be recorded."
echo "==============="
sleep 2

echo "=== 2. Starting trade_bot in the background ==="
mkdir -p logs
# Run in background
./build/release/bin/trade_bot --config config.toml > logs/trade_bot_stdout.log 2>&1 &
BOT_PID=$!

# Ensure bot is stopped on script exit
cleanup() {
    echo "Cleaning up..."
    if kill -0 $BOT_PID 2>/dev/null; then
        echo "Stopping trade_bot (PID $BOT_PID)..."
        kill $BOT_PID
    fi
}
trap cleanup EXIT

echo "Waiting 5 seconds for bot to initialize..."
sleep 5

# Check if process is still running
if ! kill -0 $BOT_PID 2>/dev/null; then
    echo "Error: trade_bot failed to start. Check logs/trade_bot_stdout.log"
    exit 1
fi

echo "=== 3. Starting DumpRecorder via Dashboard API ==="
START_RESP=$(curl -s -X POST http://127.0.0.1:$PORT/api/dump/start \
    -H "Content-Type: application/json" \
    -d "{\"filename\": \"$DUMP_NAME\"}")

echo "Response from API: $START_RESP"

if [[ $START_RESP != *"\"ok\":true"* ]]; then
    echo "Error: Failed to start recorder via dashboard API."
    exit 1
fi

echo "=== 4. Recording dump for $((DURATION_SEC/3600)) hours ($DURATION_SEC seconds) ==="
echo "Dump will be saved to: replay/dumps/${DUMP_NAME}.ndjson"

# Countdown timer
for ((i=$DURATION_SEC; i>0; i-=10)); do
    echo -ne "Time remaining: $((i/3600))h $(((i%3600)/60))m $((i%60))s \r"
    sleep 10
done
echo -e "\nRecording duration completed!"

echo "=== 5. Stopping DumpRecorder ==="
STOP_RESP=$(curl -s -X POST http://127.0.0.1:$PORT/api/dump/stop)
echo "Response from API: $STOP_RESP"

echo "=== 6. Exiting trade_bot ==="
# Trapped cleanup will stop the bot

echo "Done! The dump file is ready at: replay/dumps/${DUMP_NAME}.ndjson"
