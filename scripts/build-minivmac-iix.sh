#!/bin/sh
# Build minivmac with Mac IIx config — provides a reference oracle for
# comparing SE/30 boot behavior against our emulator.
#
# Usage: ./scripts/build-minivmac-iix.sh
# Produces: third_party/minivmac/minivmac (binary expects MacIIx.ROM
# in its cwd — NOT vMac.ROM; the setup tool hardcodes the filename
# from -m IIx).
#
# Run headlessly with: SDL_VIDEODRIVER=dummy ./third_party/minivmac/minivmac
# But note: with dummy SDL and any ROM-loading abnormality, minivmac
# silently exits without printing — for diagnostic output, build with
# -dbg 1 (enables dbglog.txt) or run with a real X display.
set -e
cd "$(dirname "$0")/../third_party/minivmac"

[ -x ./setup_t ] || gcc -o setup_t setup/tool.c

# IIx model (Mac II + 68030 + ADB + NuBus video), SDL2 API.
./setup_t -t lx64 -m IIx -api sd2 -mem 8M -em-cpu 2 > /tmp/setup_iix.sh
bash /tmp/setup_iix.sh >/dev/null

# Copy our IIx ROM to the filename minivmac expects (set by -m IIx).
[ -f ../../roms/MacIIx.ROM ] && cp ../../roms/MacIIx.ROM MacIIx.ROM

make -j 2>&1 | grep -E "error|Error" | head -5 || true
[ -f minivmac ] && echo "Built third_party/minivmac/minivmac ($(stat -c %s minivmac) bytes)"
