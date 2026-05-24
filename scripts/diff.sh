#!/bin/sh
# Triple-differential helper — run the JIT vs the reference interpreter
# block-by-block on the speedo-bench snapshot, report the first state
# divergence. Per the project's "always compare JIT to interp" SOP,
# run this after any JIT-affecting change.
#
# Usage: ./scripts/diff.sh [max-cycles]
#
#   ./scripts/diff.sh              # default 11000 cycles (just before
#                                  # the documented VIA-tick artifact
#                                  # at cycle ~11898)
#   ./scripts/diff.sh 500000       # full bench-style run; will trip
#                                  # the VIA-tick divergence — useful
#                                  # for confirming the divergence is
#                                  # timing-only (VIA-only diffs)
#
# Exit 0 = JIT and interp agree block-by-block (modulo the documented
# VIA-tick artifact). Non-zero = a real divergence, see the block
# disassembly in the output.
set -e
cd "$(dirname "$0")/.."

CYCLES=${1:-11000}

SNAP=roms/disks/speedo-bench.snap

[ -f build/mac68k_host ] || { echo "build/mac68k_host missing — run scripts/build.sh" >&2; exit 1; }
[ -f "$SNAP" ] || { echo "$SNAP not found (see scripts/bench.sh comment)" >&2; exit 1; }

echo "diff-jit-trace: cycles=$CYCLES (mask IRQs to keep engines on the same path)"
exec ./build/mac68k_host \
    --diff-jit-trace --no-irq \
    --load-snapshot "$SNAP" \
    --max-cycles "$CYCLES"
