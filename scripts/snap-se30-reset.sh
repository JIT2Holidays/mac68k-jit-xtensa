#!/bin/sh
# Regenerate the SE/30 reset snapshot (roms/disks/se30-reset.snap).
#
# Unlike the Plus boot snapshots (cycle30m/cycle100m/etc.) which capture
# the machine some way into boot, the SE/30 snapshot is taken AT RESET
# (cycle 0). It exists purely to anchor the diff_jit_trace harness on
# an SE/30 boot — verifying the JIT matches the interp instruction by
# instruction from the very first opcode.
#
# RAM size: 8 MB (small enough for git annex-style locally cached test
# data; large enough that the ROM's RAM-probe path takes the SE/30
# branch). The lockstep test runs 20K cycles which is well past the
# initial PMMU + BERR-probe sequence that drove M7.6ad/ae.
#
# Usage: ./scripts/snap-se30-reset.sh
set -e
cd "$(dirname "$0")/.."

[ -f build/se30_trace ] || tools/build_se30_trace.sh >/dev/null
[ -f roms/MacIIx.ROM ]  || { echo "roms/MacIIx.ROM not found" >&2; exit 1; }

mkdir -p roms/disks
echo "Generating SE/30 reset snapshot..."
SE30_RAM_MB=8 \
SE30_SAVE_RESET=roms/disks/se30-reset.snap \
    ./build/se30_trace 1000 >/dev/null 2>&1

[ -f roms/disks/se30-reset.snap ] || { echo "snapshot not produced" >&2; exit 1; }

echo "Verifying 20K diff_jit_trace..."
result=$(./build/mac68k_host --diff-jit-trace --no-irq \
    --load-snapshot roms/disks/se30-reset.snap --max-cycles 20000 \
    --machine se30 2>&1 \
    | grep -E "match through|DIVERGENCE" | head -1)
case "$result" in
    *"match through"*) echo "  OK — $result" ;;
    *) echo "  FAIL — $result" >&2; exit 1 ;;
esac

echo "Done. se30-reset.snap ready ($(stat -c %s roms/disks/se30-reset.snap) bytes)."
