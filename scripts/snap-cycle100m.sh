#!/bin/sh
# Regenerate the cycle100m mid-boot snapshot
# (roms/disks/boot-cycle100m.snap).
#
# This snapshot captures Mac Plus boot at cycle 100M — between the
# existing boot-rom-init (4M, RAM-test loop) and boot-system-load
# (60M, post-System-file-load). Cycle 100M sits in mid-System-startup
# (INIT/extension load phase). Hot opcodes:
#   * 0x09D1 BCHG D4,(A1)   — 688 hits / 100M (low-mem bit ops)
#   * 0x20A0 MOVE.L (A0)+,(A0)  — 193 hits (self-overlapping copy)
#   * 0x28D8 MOVE.L (A0)+,(A4)+ — 81 hits (mem-to-mem copy)
#   * 0x08D1 BSET D4,(A1)   — 61 hits
#
# Passes the full 11K --diff-jit-trace --no-irq window.
#
# Usage: ./scripts/snap-cycle100m.sh
set -e
cd "$(dirname "$0")/.."

[ -f build/mac68k_host ]            || { echo "build/mac68k_host missing — run scripts/build.sh" >&2; exit 1; }
[ -f roms/MacPlus.ROM ]             || { echo "roms/MacPlus.ROM not found" >&2; exit 1; }
[ -f roms/disks/System6.dsk ]       || { echo "roms/disks/System6.dsk not found" >&2; exit 1; }
[ -f roms/disks/InfiniteHD6.dsk ]   || { echo "roms/disks/InfiniteHD6.dsk not found" >&2; exit 1; }

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

# Trivial mouse script — only needed to enter scripted-run mode.
cat > "$TMP/script.mscript" <<MSCR
500000000  200 200 0
MSCR

echo "Generating cycle100m snapshot (target cycle 100M)..."
MAC68K_MOUSESCRIPT="$TMP/script.mscript" \
MAC68K_FRAMEDIR="$TMP" \
MAC68K_FRAME_EVERY=999999999 \
MAC68K_DISK2_CYCLE=999999999 \
MAC68K_SNAP="$TMP/snap.snap" \
MAC68K_SNAP_CYCLE=100000000 \
MAC68K_END_CYCLE=120000000 \
    ./build/mac68k_host --server \
        --rom roms/MacPlus.ROM \
        --disk roms/disks/System6.dsk \
        --disk roms/disks/InfiniteHD6.dsk \
        --ram-mb 4 \
        --max-cycles 120000000 \
        --interp >/dev/null 2>&1

[ -f "$TMP/snap.snap" ] || { echo "snapshot not produced" >&2; exit 1; }
cp "$TMP/snap.snap" roms/disks/boot-cycle100m.snap

echo "Verifying 11K diff_jit_trace..."
result=$(./build/mac68k_host --diff-jit-trace --no-irq \
    --load-snapshot roms/disks/boot-cycle100m.snap --max-cycles 11000 2>&1 \
    | grep -E "match through|DIVERGENCE" | head -1)
case "$result" in
    *"match through"*) echo "  OK — $result" ;;
    *) echo "  FAIL — $result" >&2; exit 1 ;;
esac

echo "Done. boot-cycle100m.snap ready."
