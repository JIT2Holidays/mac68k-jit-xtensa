#!/bin/sh
# Build minivmac with Mac IIx config — provides a reference oracle for
# comparing SE/30 boot behavior against our emulator.
#
# Usage: ./scripts/build-minivmac-iix.sh
# Produces: third_party/minivmac/minivmac
# Run headlessly with: SDL_VIDEODRIVER=dummy ./third_party/minivmac/minivmac
# (after copying roms/MacIIx.ROM to third_party/minivmac/vMac.ROM)
set -e
cd "$(dirname "$0")/../third_party/minivmac"

[ -x ./setup_t ] || gcc -o setup_t setup/tool.c

# IIx model (Mac II + 68030 + ADB + NuBus video), SDL2 API.
./setup_t -t lx64 -m IIx -api sd2 -mem 8M -em-cpu 2 > /tmp/setup_iix.sh
bash /tmp/setup_iix.sh >/dev/null

make -j 2>&1 | grep -E "error|Error" | head -5 || true
[ -f minivmac ] && echo "Built third_party/minivmac/minivmac ($(stat -c %s minivmac) bytes)"
