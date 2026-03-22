#!/bin/bash
set -e

echo "Configuring librim project..."
cmake -B build -S .

echo "Building project..."
cmake --build build -j$(nproc)

echo "Build complete."
echo "   Run tests: (cd build && ctest)"
echo "   Run demo:  ./build/app/librim_demo"
