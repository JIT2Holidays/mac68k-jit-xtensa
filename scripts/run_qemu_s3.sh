#!/usr/bin/env bash
# Build the ESP32-S3 firmware and run it under qemu-system-xtensa.
#
# The firmware translates the built-in 68000 demo to Xtensa machine code
# and executes it on the emulated LX7 core. Exit code 0 iff the run
# prints "RESULT: PASS".
#
# Usage: ./scripts/run_qemu_s3.sh [--timeout SECONDS]
# Requires ESP-IDF to be sourced (idf.py on PATH).

# Note: deliberately not `set -e` — the IDF activate script trips it.
set -uo pipefail

TIMEOUT=40
if [[ "${1:-}" == "--timeout" ]]; then TIMEOUT="$2"; shift 2; fi

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PORT_DIR="$REPO/port/esp32s3"
BUILD="$PORT_DIR/build"

# Bring up ESP-IDF v6.0.1 (handles the venv-path mismatch — see idf_env.sh).
source "$REPO/scripts/idf_env.sh"

echo "[run_qemu_s3] building..."
idf.py -C "$PORT_DIR" build >/tmp/mac68k_build.log 2>&1 \
  || { echo "BUILD FAILED:"; tail -40 /tmp/mac68k_build.log; exit 3; }
echo "[run_qemu_s3] build OK"

# Merge bootloader + partition table + app into a single flash image.
rm -f "$BUILD/qemu_flash.bin"
esptool --chip=esp32s3 merge-bin \
    --output="$BUILD/qemu_flash.bin" --pad-to-size=4MB \
    --flash-mode dio --flash-freq 80m --flash-size 4MB \
    0x0     "$BUILD/bootloader/bootloader.bin" \
    0x8000  "$BUILD/partition_table/partition-table.bin" \
    0x10000 "$BUILD/mac68k_s3.bin" >/dev/null

LOG=$(mktemp)
echo "[run_qemu_s3] running qemu-system-xtensa (timeout ${TIMEOUT}s)..."
timeout "$TIMEOUT" qemu-system-xtensa \
    -M esp32s3 -m 8M -nographic -no-reboot \
    -drive file="$BUILD/qemu_flash.bin",if=mtd,format=raw \
    -global driver=timer.esp32s3.timg,property=wdt_disable,value=true \
    -serial mon:stdio 2>&1 | tee "$LOG" | grep -E '(mac68k|BENCH|RESULT|Guru|panic|abort)' || true

if grep -q 'RESULT: PASS' "$LOG"; then
    echo "[run_qemu_s3] PASS"
    rm -f "$LOG"
    exit 0
fi
echo "[run_qemu_s3] FAILED — full log: $LOG"
exit 1
