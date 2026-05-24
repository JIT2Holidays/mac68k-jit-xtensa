#!/bin/sh
# Run the JIT (or interpreter) against the frozen Speedometer benchmark
# snapshot. Reports lx7_per_cyc — the canonical JIT-cost metric (lower
# is better; interp baseline = 6.462 lx7/cyc, JIT goal was 5× interp
# = 1.32 lx7/cyc, currently ~1.279).
#
# Usage: ./scripts/bench.sh [jit|interp] [max-cycles] [extra-args...]
#
#   ./scripts/bench.sh                              # JIT, 60M cycles
#   ./scripts/bench.sh interp                       # interp baseline
#   ./scripts/bench.sh jit 60000000 --evict lru     # try LRU eviction
#   ./scripts/bench.sh jit 60000000 --arena-kb 64   # tight arena
#
# Requires roms/disks/speedo-bench.snap (a frozen mid-benchmark
# snapshot — too large to track in git; regenerable by booting +
# scripting Speedometer in mac_gui).
set -e
cd "$(dirname "$0")/.."

MODE=${1:-jit}
CYCLES=${2:-60000000}
shift 2 2>/dev/null || shift $#

SNAP=roms/disks/speedo-bench.snap

case "$MODE" in
    jit|interp) ;;
    *) echo "first arg must be 'jit' or 'interp' (got '$MODE')" >&2; exit 1 ;;
esac

[ -f build/mac68k_host ] || { echo "build/mac68k_host missing — run scripts/build.sh" >&2; exit 1; }
[ -f "$SNAP" ] || {
    echo "$SNAP not found." >&2
    echo "  This is a frozen Speedometer mid-benchmark snapshot, not tracked" >&2
    echo "  in git. Regenerate by running Speedometer in mac_gui and using" >&2
    echo "  MAC68K_SNAP=<path> MAC68K_SNAP_CYCLE=<n> while scripted." >&2
    exit 1
}

echo "Bench: mode=$MODE cycles=$CYCLES extras=$*"
exec ./build/mac68k_host \
    "--$MODE" \
    --load-snapshot "$SNAP" \
    --max-cycles "$CYCLES" \
    "$@"
