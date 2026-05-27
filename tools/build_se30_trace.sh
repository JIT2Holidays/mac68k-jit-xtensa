#!/bin/sh
# Build the SE/30 boot tracer (tools/se30_trace.c).
set -e
cd "$(dirname "$0")/.."
mkdir -p build/se30trace
for f in core/m68k_interp core/mac_mem core/mac_iwm core/mac_input core/sony core/mac_scsi core/mac_snd; do
    cc -c -O2 -Iinclude -Icore "$f.c" -o "build/se30trace/$(basename $f).o"
done
cc -c -O2 -Iinclude -Icore tools/se30_trace.c -o build/se30trace/se30_trace.o
cc -o build/se30_trace build/se30trace/*.o -lm
echo "Built build/se30_trace"
