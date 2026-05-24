#!/bin/sh
# Launch the SDL GUI front-end with the Mac Plus ROM + a boot disk.
# Usage: ./scripts/gui.sh [rom] [disk]
#
# Defaults: ROM = roms/MacPlus.ROM, disk = roms/disks/System6.dsk.
# Override via positional args, e.g.:
#   ./scripts/gui.sh roms/MacPlus.ROM roms/disks/ssw608_d1.img
#
# The InfiniteHD6.dsk (if present at roms/disks/InfiniteHD6.dsk) is
# auto-inserted as drive 2 a moment after boot — the GUI / host
# pipeline handles that internally (see port/host/main.c).
set -e
cd "$(dirname "$0")/.."

ROM=${1:-roms/MacPlus.ROM}
DISK=${2:-roms/disks/System6.dsk}

[ -f build/mac_gui ] || { echo "build/mac_gui missing — run scripts/build.sh" >&2; exit 1; }
[ -f "$ROM" ] || { echo "ROM not found: $ROM" >&2; exit 1; }
[ -f "$DISK" ] || { echo "disk not found: $DISK" >&2; exit 1; }

echo "Launching GUI:  rom=$ROM  disk=$DISK"
exec ./build/mac_gui "$ROM" "$DISK"
