#!/bin/sh
# Headless boot of the Mac Plus ROM, prints perf stats at the end.
# Usage: ./scripts/boot.sh [jit|interp] [max-cycles] [extra-args...]
#
#   ./scripts/boot.sh                    # JIT, 100M cycles (default)
#   ./scripts/boot.sh interp             # interpreter, 100M cycles
#   ./scripts/boot.sh jit 300000000      # JIT, 300M cycles
#   ./scripts/boot.sh jit 100000000 --evict lru --arena-kb 256
#
# Auto-mounts roms/disks/InfiniteHD6.dsk as drive 2 if present.
set -e
cd "$(dirname "$0")/.."

MODE=${1:-jit}
CYCLES=${2:-100000000}
shift 2 2>/dev/null || shift $#

case "$MODE" in
    jit|interp) ;;
    *) echo "first arg must be 'jit' or 'interp' (got '$MODE')" >&2; exit 1 ;;
esac

[ -f build/mac68k_host ] || { echo "build/mac68k_host missing — run scripts/build.sh" >&2; exit 1; }
[ -f roms/MacPlus.ROM ] || { echo "roms/MacPlus.ROM not found" >&2; exit 1; }

echo "Boot: mode=$MODE cycles=$CYCLES extras=$*"
exec ./build/mac68k_host \
    "--$MODE" \
    --rom roms/MacPlus.ROM \
    --max-cycles "$CYCLES" \
    "$@"
