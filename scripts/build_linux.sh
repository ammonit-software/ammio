#!/bin/sh
# Usage: scripts/build_linux.sh [Debug|Release]   (default: Release)
# Builds ammio and runs the test suite.
set -e
cd "$(dirname "$0")/.."
cmake -S . -B build -DCMAKE_BUILD_TYPE="${1:-Release}" -DAMMIO_TESTS=ON
cmake --build build -j "$(nproc 2>/dev/null || echo 4)"
(cd build && ctest --output-on-failure)
