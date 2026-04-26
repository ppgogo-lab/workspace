#!/bin/bash
# Test script for Optimization #1: Gateway Recovery Lookup Removal
# Measures latency improvement from lock-free state allocation

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="$PROJECT_ROOT/build-wsl"

echo "=========================================="
echo "Optimization #1 Latency Test"
echo "=========================================="
echo ""

# Check if build exists
if [ ! -f "$BUILD_DIR/test_latency" ]; then
    echo "ERROR: test_latency not found in $BUILD_DIR"
    echo "Please build the project first:"
    echo "  cd $PROJECT_ROOT"
    echo "  mkdir -p build-wsl && cd build-wsl"
    echo "  cmake .. -DCMAKE_BUILD_TYPE=Release"
    echo "  make -j\$(nproc)"
    exit 1
fi

echo "Running latency benchmark..."
echo ""

# Run the latency test
cd "$BUILD_DIR"
./test_latency

echo ""
echo "=========================================="
echo "Test completed!"
echo "=========================================="
echo ""
echo "Key metrics to check:"
echo "  - Tick-to-trade latency (p50, p99, p99.9)"
echo "  - Order submission latency"
echo "  - Quote submission latency"
echo ""
echo "Expected improvement: 300-1300ns reduction in order/quote submission"
