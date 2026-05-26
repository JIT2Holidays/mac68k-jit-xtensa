#!/bin/sh
# Generate roms/disks/thinkc-bullseye.snap — THINK C 5.0's built-in
# "Bullseye" demo running. The demo draws a concentric-circles target
# and exercises a rich opcode mix (variable LSR, MOVE-family indexed,
# JSR/JMP through tables) at high helper rates — making it a strong
# stress for the JIT helper-bridge fast paths.
#
# Bench characteristics at the captured cycle (9.5B):
#   2.222 lx7/cyc cost-model, 3.206 real_lx7/cyc, ~2M real helpers/100M
#   Top opcodes: LSR.L D5,D0 (210K), MOVE.W (d8,A6,Xn),D6 (132K),
#   MOVE.B (A0)+,D0 (111K), MOVE.L (d8,A6,Xn),D6 (97K), and dozens more.
#
# Like thinkc8-folder-open.snap, this snapshot is NOT JIT-deterministic
# against the interpreter beyond a few hundred cycles — the bullseye
# demo polls the VIA timer for animation, and JIT vs interp per-block
# vs per-instruction tick granularity diverges quickly. BENCH numbers
# are meaningful; diff_jit_trace is not.
#
# Usage: ./scripts/snap-thinkc-bullseye.sh
# Output: roms/disks/thinkc-bullseye.snap (cycle 9.5B, pc=0x401F72)
#
# Recipe — extends [[finder-navigation-cookbook]]:
#   1. Boot from System6.0.5.dsk, insert InfiniteHD6.dsk at 0.5B cyc.
#   2. Double-click Infinite HD at (465, 170)        — open desktop.
#   3. Double-click Developer at (228, 119)          — open Developer.
#   4. Drag vertical scroll thumb (412, 132)→(412, 215) — reveal THINK C.
#   5. Double-click THINK C 8 at (205, 202)          — open folder.
#   6. View menu → by Name (135,10 hold, drag (135,56))— switch to list view.
#   7. Double-click THINK C 5.0 at (330, 305)        — launch IDE.
#   8. IDE opens Project file dialog. Double-click THINK Demos at
#      (150, 185) — drill into demos folder.
#   9. Double-click Bullseye Folder at (150, 105)    — drill into demo.
#  10. Double-click bullseye.π at (150, 105)         — open project.
#  11. Cmd-R (keycode 0x37+0x0F) at cycle 5B          — Run the demo.
#  12. Snapshot at cycle 9.5B, after compile+link+run is complete and
#      the bullseye target is being drawn.
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
# === View menu → by Name (switch to list view) ===
1700000000  135 10 0
1702000000  135 10 1
1750000000  135 56 1
1800000000  135 56 0
# === Double-click THINK C 5.0 (launch IDE) ===
1900000000  330 305 0
1901000000  330 305 1
1902000000  330 305 0
1904000000  330 305 1
1905000000  330 305 0
# === Double-click THINK Demos in IDE's Open Project dialog ===
3000000000  150 185 0
3001000000  150 185 1
3002000000  150 185 0
3004000000  150 185 1
3005000000  150 185 0
# === Double-click Bullseye Folder ===
3500000000  150 105 0
3501000000  150 105 1
3502000000  150 105 0
3504000000  150 105 1
3505000000  150 105 0
# === Double-click bullseye.π (the project file) ===
4000000000  150 105 0
4001000000  150 105 1
4002000000  150 105 0
4004000000  150 105 1
4005000000  150 105 0
# === Cmd-R to Run the project ===
k 5000000000 0x37 1
k 5001000000 0x0F 1
k 5005000000 0x0F 0
k 5006000000 0x37 0
EOF

echo "Generating THINK C Bullseye snapshot (target cycle 9.5B, runs ~10s wall)..."
MAC68K_MOUSESCRIPT="$TMP/script.mscript" \
MAC68K_FRAMEDIR="$TMP" \
MAC68K_FRAME_EVERY=999999999 \
MAC68K_DISK2_CYCLE=500000000 \
MAC68K_END_CYCLE=10000000000 \
MAC68K_SNAP="$TMP/thinkc-bullseye.snap" \
MAC68K_SNAP_CYCLE=9500000000 \
    ./build/mac68k_host --server \
        --rom roms/MacPlus.ROM \
        --disk roms/disks/System6.0.5.dsk \
        --disk roms/disks/InfiniteHD6.dsk \
        --ram-mb 4 \
        --max-cycles 10000000000 \
        --interp >/dev/null 2>&1

[ -f "$TMP/thinkc-bullseye.snap" ] || { echo "snapshot not produced" >&2; exit 1; }
cp "$TMP/thinkc-bullseye.snap" roms/disks/thinkc-bullseye.snap

echo "Verifying snapshot via 100M-cycle JIT bench..."
result=$(./build/mac68k_host --jit \
    --load-snapshot roms/disks/thinkc-bullseye.snap \
    --max-cycles 100000000 2>&1 | grep '^\[BENCH\]')
echo "  $result"

case "$result" in
    *lx7_per_cyc=2.*) echo "Done. Snapshot ready for bench rotation." ;;
    *) echo "Unexpected bench result — drift from expected ~2.2 lx7/cyc" >&2 ;;
esac
