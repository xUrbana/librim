#!/bin/bash

# Configuration defaults
PROTOCOL=${1:-tcp}
HOST=${2:-127.0.0.1}
PORT=${3:-8081}
DURATION=${4:-5}
CLIENTS=${5:-50}

# Build directories dynamically parsed guaranteeing successful execution paths locally
BUILD_DIR="$(dirname "$0")/../build/benchmark"

if [ ! -f "$BUILD_DIR/librim_bench_server" ] || [ ! -f "$BUILD_DIR/librim_bench_client" ]; then
    echo "Error: Benchmark binaries not found!"
    echo "Please run: 'cd build && make' first."
    exit 1
fi

echo "================================================="
echo " Starting Local Independent Benchmarks ($PROTOCOL) "
echo " Target: $HOST:$PORT for $DURATION seconds       "
echo "================================================="
echo ""

echo "[*] Spinning up the Benchmark Server in the background loop..."
$BUILD_DIR/librim_bench_server $PROTOCOL $PORT &
SERVER_PID=$!

# Give the server a second to physically bind its socket native routing loops completely
sleep 1
echo ""

echo "[*] Launching extreme-throughput Clients!"
$BUILD_DIR/librim_bench_client $PROTOCOL $HOST $PORT $DURATION $CLIENTS

echo ""
echo "[*] Test cleanly concluding. Terminating background Server orchestrations..."
kill -SIGTERM $SERVER_PID
wait $SERVER_PID 2>/dev/null

echo "[✓] Complete!"
