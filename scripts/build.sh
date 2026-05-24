#!/bin/sh
# Build the host emulator + tests in Release mode.
# Usage: ./scripts/build.sh [extra-cmake-args...]
#
# Idempotent — configure step is a no-op once the build dir exists.
set -e
cd "$(dirname "$0")/.."

[ -d build ] || cmake -B build -DCMAKE_BUILD_TYPE=Release "$@"
cmake --build build -j8

echo
echo "Built:"
echo "  ./build/mac68k_host    — headless CLI (interp / JIT)"
echo "  ./build/mac_gui        — SDL GUI front-end"
echo "  ./build/test_*         — ctest binaries (run scripts/test.sh)"
