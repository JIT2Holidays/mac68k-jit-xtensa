#!/bin/sh
# Run the ctest suite.
# Usage: ./scripts/test.sh [extra-ctest-args...]
#
# Includes the JIT/interp differential and the lockstep diff-jit-trace
# against speedo-bench.snap (if present locally — auto-skipped on a
# fresh checkout that doesn't have the snapshot).
set -e
cd "$(dirname "$0")/.."

[ -d build ] || { echo "build/ not found — run scripts/build.sh first" >&2; exit 1; }
ctest --test-dir build --output-on-failure "$@"
