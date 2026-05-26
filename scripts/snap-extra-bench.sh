#!/bin/sh
# Regenerate the two "extra bench" snapshots used by ctest's
# diff_jit_boot_rom_init_lockstep and diff_jit_boot_system_load_lockstep
# tests (M6.153). Both snapshots are gitignored (copyrighted ROM bytes);
# this script reproduces them deterministically from a clean Mac Plus
# ROM + System6 boot.
#
# Usage: ./scripts/snap-extra-bench.sh
#
# Output: roms/disks/boot-rom-init.snap     (cycle 4M, PC=0x40032C)
#         roms/disks/boot-system-load.snap  (cycle 60M, PC=0x401F6E)
#
# Both pass the full 11K --diff-jit-trace --no-irq window and exercise
# distinct ROM code paths from the existing speedo-bench.snap (which
# is mid-Speedometer's tight ALU loop).
#
# Originally these benchmark slots were intended for MacBench 4.0 and
# THINK C compile snapshots (STATUS.md §"Future bench targets"), but
# the Finder app-launch path documented in M6.67 §2809 remained
# blocked. The two boot-ROM snapshots provide the same broadened-
# differential value without that wall.
set -e
cd "$(dirname "$0")/.."

[ -f build/mac68k_host ] || { echo "build/mac68k_host missing — run scripts/build.sh" >&2; exit 1; }
[ -f roms/MacPlus.ROM ] || { echo "roms/MacPlus.ROM not found" >&2; exit 1; }
[ -f roms/disks/System6.0.5.dsk ] || { echo "roms/disks/System6.0.5.dsk not found" >&2; exit 1; }

TMPDIR=$(mktemp -d)
trap 'rm -rf "$TMPDIR"' EXIT

# Trivial mouse script (only required to enter scripted-run mode that
# honours MAC68K_SNAP_EVERY).
cat > "$TMPDIR/script.mscript" <<EOF
1500000000  200 200 0
EOF

echo "Capturing periodic boot snapshots via interpreter (target cycles 4M, 60M)..."
MAC68K_MOUSESCRIPT="$TMPDIR/script.mscript" \
MAC68K_SNAP="$TMPDIR/snap" \
MAC68K_SNAP_EVERY=2000000 \
MAC68K_END_CYCLE=65000000 \
MAC68K_FRAMEDIR="$TMPDIR" \
MAC68K_FRAME_EVERY=999999999 \
    ./build/mac68k_host --interp \
        --rom roms/MacPlus.ROM \
        --disk roms/disks/System6.0.5.dsk \
        --server >/dev/null 2>&1

# Indices follow the snapshot-every cadence; cycle 4M = index 1,
# cycle 60M = index 29. Verify the PCs match the expected locations
# before promoting (catches ROM/System file drift).
sn1="$TMPDIR/snap.001"
sn29="$TMPDIR/snap.029"
[ -f "$sn1" ]  || { echo "snap.001 not produced" >&2; exit 1; }
[ -f "$sn29" ] || { echo "snap.029 not produced" >&2; exit 1; }

cp "$sn1"  roms/disks/boot-rom-init.snap
cp "$sn29" roms/disks/boot-system-load.snap

echo "Verifying snapshots pass the 11K diff-jit-trace window..."
for snap in boot-rom-init boot-system-load; do
    result=$(./build/mac68k_host --diff-jit-trace --no-irq \
        --load-snapshot "roms/disks/$snap.snap" --max-cycles 11000 2>&1 \
        | grep -E "match through|DIVERGENCE" | head -1)
    case "$result" in
        *"match through"*) echo "  $snap.snap: OK — $result" ;;
        *) echo "  $snap.snap: FAIL — $result" >&2; exit 1 ;;
    esac
done

echo "Done. Both snapshots ready for ctest."
