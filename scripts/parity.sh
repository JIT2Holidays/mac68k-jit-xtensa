#!/bin/sh
# JIT-vs-interp framebuffer parity check across several cycle budgets.
# Runs both engines on the live Mac Plus boot and byte-compares the
# resulting screenshot BMPs. A mismatch at any cycle count is a real
# JIT correctness bug.
#
# Usage:  ./scripts/parity.sh [cycles_list]
#         cycles_list defaults to "100M 500M 1B 2B 3B"
set -e
cd "$(dirname "$0")/.."

[ -f build/mac68k_host ]            || { echo "build/mac68k_host missing" >&2; exit 1; }
[ -f roms/MacPlus.ROM ]             || { echo "roms/MacPlus.ROM not found" >&2; exit 1; }
[ -f roms/disks/System6.dsk ]       || { echo "roms/disks/System6.dsk not found" >&2; exit 1; }

if [ $# -gt 0 ]; then CYCS="$*"
else                  CYCS="100000000 500000000 1000000000 2000000000 3000000000"
fi
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

fail=0
for cyc in $CYCS; do
    ./build/mac68k_host --jit    --rom roms/MacPlus.ROM \
        --disk roms/disks/System6.dsk --ram-mb 4 \
        --max-cycles "$cyc" --screenshot "$TMP/j.bmp" \
        --audio "$TMP/j.raw" >/dev/null 2>&1
    ./build/mac68k_host --interp --rom roms/MacPlus.ROM \
        --disk roms/disks/System6.dsk --ram-mb 4 \
        --max-cycles "$cyc" --screenshot "$TMP/i.bmp" \
        --audio "$TMP/i.raw" >/dev/null 2>&1
    bmp_ok=0; aud_ok=0
    cmp -s "$TMP/j.bmp" "$TMP/i.bmp" && bmp_ok=1
    cmp -s "$TMP/j.raw" "$TMP/i.raw" && aud_ok=1
    if [ "$bmp_ok" = 1 ] && [ "$aud_ok" = 1 ]; then
        printf "  %sM cyc: framebuffer + audio identical\n" "$((cyc / 1000000))"
    else
        bd=$(cmp -l "$TMP/j.bmp" "$TMP/i.bmp" 2>/dev/null | wc -l)
        ad=$(cmp -l "$TMP/j.raw" "$TMP/i.raw" 2>/dev/null | wc -l)
        printf "  %sM cyc: FAIL (bmp diff %s, audio diff %s)\n" "$((cyc / 1000000))" "$bd" "$ad"
        fail=1
    fi
done

if [ "$fail" -ne 0 ]; then
    echo "PARITY FAILED"
    exit 1
fi
echo "PARITY OK"
