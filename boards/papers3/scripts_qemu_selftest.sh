#!/usr/bin/env bash
# Build the PaperS3 firmware in JIT self-test mode (no PSRAM, no SD) and run
# it under qemu-system-xtensa to validate the Xtensa JIT. PASS iff the
# interp and JIT engines agree on the demo's final framebuffer.
set -uo pipefail
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
source "$REPO/scripts/idf_env.sh"
cd "$REPO/boards/papers3"
rm -f sdkconfig
idf.py -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.qemu" \
       -DBOARD_QEMU_SELFTEST=1 build >/tmp/st_build.log 2>&1 \
  || { echo "BUILD FAILED"; tail -30 /tmp/st_build.log; exit 3; }
B=build
esptool --chip=esp32s3 merge-bin --output=$B/qemu_flash.bin --pad-to-size=8MB \
  --flash-mode dio --flash-freq 80m --flash-size 8MB \
  0x0 $B/bootloader/bootloader.bin \
  0x8000 $B/partition_table/partition-table.bin \
  0x10000 $B/mac68k_papers3.bin >/dev/null 2>&1
LOG=$(mktemp)
timeout "${1:-120}" qemu-system-xtensa -M esp32s3 -m 8M -nographic -no-reboot \
  -drive file=$B/qemu_flash.bin,if=mtd,format=raw \
  -global driver=timer.esp32s3.timg,property=wdt_disable,value=true \
  -serial mon:stdio 2>&1 | tee "$LOG" \
  | grep -E "mac68k|BENCH|JIT\]|SELFTEST|Guru Meditation|panic|abort\(|assert"
echo "=== verdict ==="
if grep -qE "Guru Meditation|panic'ed|abort\(\) was called" "$LOG"; then
    echo ">>> CRASH (firmware panic) — JIT FAIL"
elif grep -q "SELFTEST: PASS" "$LOG"; then
    echo ">>> JIT SELFTEST: PASS"
else
    echo ">>> JIT SELFTEST: FAIL / no verdict"
fi
