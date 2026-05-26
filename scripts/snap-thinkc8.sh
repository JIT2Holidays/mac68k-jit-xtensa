#!/bin/sh
# Regenerate the THINK C 8 mid-Finder snapshot
# (roms/disks/thinkc8-folder-open.snap) — a bench-only snapshot exercising
# the Finder steady-state code path with multiple overlapping windows
# open. Different opcode mix than [boot-rom-init, boot-system-load,
# speedo-bench]: ~2.1M helpers per 100M cycles vs <3K for speedo, so it
# stresses the JIT's helper-bridge fast paths heavily.
#
# This snapshot is NOT JIT-deterministic against the interpreter beyond
# step ~21 (see M6.66 trajectory traps in MEMORY.md) — only the BENCH
# numbers are meaningful, NOT diff_jit_trace.
#
# Usage: ./scripts/snap-thinkc8.sh
# Output: roms/disks/thinkc8-folder-open.snap (cycle 1.9B, pc=0x3E97C0)
#
# Recipe (see [[finder-navigation-cookbook]] in agent memory):
#   1. Boot from System6.0.5.dsk, with InfiniteHD6.dsk inserted at 0.5B cyc.
#   2. Double-click the Infinite HD icon at (465, 170) at 0.6B.
#   3. Double-click the 'Developer' folder at (228, 119) at 0.9B.
#   4. Drag the vertical scroll thumb from (412, 132) to (412, 215),
#      which jumps the icon list past the M-R items into the T-Z range.
#   5. Double-click 'THINK C 8' at (205, 202) at 1.3B.
#   6. Snapshot the resulting Finder state at 1.9B.
#
# All click coordinates determined by tesseract-on-framebuffer OCR
# (see [[finder-navigation-cookbook]] memory note for details).
set -e
cd "$(dirname "$0")/.."

[ -f build/mac68k_host ]            || { echo "build/mac68k_host missing — run scripts/build.sh" >&2; exit 1; }
[ -f roms/MacPlus.ROM ]             || { echo "roms/MacPlus.ROM not found" >&2; exit 1; }
[ -f roms/disks/System6.0.5.dsk ]       || { echo "roms/disks/System6.0.5.dsk not found" >&2; exit 1; }
[ -f roms/disks/InfiniteHD6.dsk ]   || { echo "roms/disks/InfiniteHD6.dsk not found" >&2; exit 1; }

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

cat > "$TMP/script.mscript" <<'EOF'
# === Open Infinite HD ===
600000000   465 170 0
601000000   465 170 1
602000000   465 170 0
604000000   465 170 1
605000000   465 170 0
# === Double-click Developer ===
900000000   228 119 0
901000000   228 119 1
902000000   228 119 0
904000000   228 119 1
905000000   228 119 0
# === Drag scroll thumb to bottom ===
1100000000  412 132 0
1101000000  412 132 1
1102000000  412 140 1
1103000000  412 160 1
1104000000  412 180 1
1105000000  412 200 1
1106000000  412 215 1
1107000000  412 215 0
# === Double-click THINK C 8 ===
1300000000  205 202 0
1301000000  205 202 1
1302000000  205 202 0
1304000000  205 202 1
1305000000  205 202 0
EOF

echo "Generating THINK C 8 snapshot (target cycle 1.9B, runs ~3s wall)..."
MAC68K_MOUSESCRIPT="$TMP/script.mscript" \
MAC68K_FRAMEDIR="$TMP" \
MAC68K_FRAME_EVERY=999999999 \
MAC68K_DISK2_CYCLE=500000000 \
MAC68K_END_CYCLE=2000000000 \
MAC68K_SNAP="$TMP/thinkc8.snap" \
MAC68K_SNAP_CYCLE=1900000000 \
    ./build/mac68k_host --server \
        --rom roms/MacPlus.ROM \
        --disk roms/disks/System6.0.5.dsk \
        --disk roms/disks/InfiniteHD6.dsk \
        --ram-mb 4 \
        --max-cycles 2000000000 \
        --interp >/dev/null 2>&1

[ -f "$TMP/thinkc8.snap" ] || { echo "snapshot not produced" >&2; exit 1; }
cp "$TMP/thinkc8.snap" roms/disks/thinkc8-folder-open.snap

echo "Verifying snapshot via 100M-cycle JIT bench..."
result=$(./build/mac68k_host --jit \
    --load-snapshot roms/disks/thinkc8-folder-open.snap \
    --max-cycles 100000000 2>&1 | grep '^\[BENCH\]')
echo "  $result"

case "$result" in
    *lx7_per_cyc=2.*) echo "Done. Snapshot ready for bench rotation." ;;
    *) echo "Unexpected bench result — drift from expected ~2.4 lx7/cyc" >&2 ;;
esac
