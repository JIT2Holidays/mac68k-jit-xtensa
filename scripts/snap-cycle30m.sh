#!/bin/sh
# Regenerate the cycle30m boot snapshot
# (roms/disks/boot-cycle30m.snap).
#
# Captures Mac Plus boot at cycle 30M — between boot-rom-init (4M)
# and boot-system-load (60M). Falls in the post-RAM-test, pre-System-
# file phase. Hot opcodes are mostly MULU/DIVU (multiply/divide
# during Toolbox init) at single-digit fire counts.
#
# Distinctive characteristic: very low helper count (8 per 100M cyc)
# and lowest lx7/cyc across all six benches (1.334). The boot ROM
# trap dispatcher is running with rarely-helper-bridging ops.
#
# Deterministic to ≥10M cycles (111K blocks). Run 100K in ctest.
#
# Usage: ./scripts/snap-cycle30m.sh
set -e
cd "$(dirname "$0")/.."

[ -f build/mac68k_host ]            || { echo "build/mac68k_host missing — run scripts/build.sh" >&2; exit 1; }
[ -f roms/MacPlus.ROM ]             || { echo "roms/MacPlus.ROM not found" >&2; exit 1; }
[ -f roms/disks/System6.0.5.dsk ]       || { echo "roms/disks/System6.0.5.dsk not found" >&2; exit 1; }
[ -f roms/disks/InfiniteHD6.dsk ]   || { echo "roms/disks/InfiniteHD6.dsk not found" >&2; exit 1; }

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

cat > "$TMP/script.mscript" <<MSCR
500000000  200 200 0
MSCR

echo "Generating cycle30m snapshot (target cycle 30M)..."
MAC68K_MOUSESCRIPT="$TMP/script.mscript" \
MAC68K_FRAMEDIR="$TMP" \
MAC68K_FRAME_EVERY=999999999 \
MAC68K_DISK2_CYCLE=999999999 \
MAC68K_SNAP="$TMP/snap.snap" \
MAC68K_SNAP_CYCLE=30000000 \
MAC68K_END_CYCLE=40000000 \
    ./build/mac68k_host --server \
        --rom roms/MacPlus.ROM \
        --disk roms/disks/System6.0.5.dsk \
        --disk roms/disks/InfiniteHD6.dsk \
        --ram-mb 4 \
        --max-cycles 40000000 \
        --interp >/dev/null 2>&1

[ -f "$TMP/snap.snap" ] || { echo "snapshot not produced" >&2; exit 1; }
cp "$TMP/snap.snap" roms/disks/boot-cycle30m.snap

echo "Verifying 100K diff_jit_trace..."
result=$(./build/mac68k_host --diff-jit-trace --no-irq \
    --load-snapshot roms/disks/boot-cycle30m.snap --max-cycles 100000 2>&1 \
    | grep -E "match through|DIVERGENCE" | head -1)
case "$result" in
    *"match through"*) echo "  OK — $result" ;;
    *) echo "  FAIL — $result" >&2; exit 1 ;;
esac

echo "Done. boot-cycle30m.snap ready."
