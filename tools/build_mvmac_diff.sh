#!/bin/sh
# Build the mini vMac differential harness — an instruction-lockstep
# comparison of this project's 68000 interpreter against mini vMac's CPU
# core, used to pinpoint emulation bugs (it found the ABCD/SBCD/NBCD
# discrepancy behind the Control Panel corruption).
set -e
cd "$(dirname "$0")/.."
ROOT=$(pwd)
MV=third_party/minivmac/src
CFG=/tmp/mvbuild/cfg
OUT=build/mvmac_diff
mkdir -p build/mvdiff

# Generate mini vMac's config headers once (its setup tool emits them
# for a Macintosh Plus build).
if [ ! -f "$CFG/CNFUIALL.h" ]; then
    echo "generating mini vMac config..."
    cc -o /tmp/mvmac_setup_t third_party/minivmac/setup/tool.c
    /tmp/mvmac_setup_t -m Plus -api sdl -t mc64 > /tmp/mvmac_setup.sh
    mkdir -p /tmp/mvbuild
    ( cd /tmp/mvbuild && my_project_d="./" bash /tmp/mvmac_setup.sh )
fi

# mini vMac CPU core (compiled quietly — it is third-party code)
cc -c -O1 -w -I"$CFG" -I"$MV" tools/minem_glue.c       -o build/mvdiff/minem_glue.o
cc -c -O1 -w -I"$CFG" -I"$MV" "$MV/M68KITAB.c"         -o build/mvdiff/m68kitab.o

# this project's core + the harness
for f in core/m68k_interp core/mac_mem core/mac_iwm core/mac_input core/sony; do
    cc -c -O1 -Iinclude -Icore "$f.c" -o "build/mvdiff/$(basename $f).o"
done
cc -c -O1 -Iinclude -Icore tools/mvmac_diff.c -o build/mvdiff/mvmac_diff.o

cc build/mvdiff/*.o -o "$OUT"
echo "built $OUT"
