#!/bin/sh
# M6.63 — sweep arena size across all three eviction policies on the
# bench AND boot workloads. Output one row per (workload, policy, kb)
# to a CSV on stdout: workload,policy,arena_kb,lx7_per_cyc,real_lx7_per_cyc,blocks_compiled,interp_fallbacks,smc_invalidations,arena_resets.
#
# The interp baseline (single row) is added at the end.
set -e
cd "$(dirname "$0")/.."

BIN=./build/mac68k_host
BENCH_SNAP=roms/disks/speedo-bench.snap
ROM=roms/macplus.rom
BENCH_CYCLES=60000000
BOOT_CYCLES=100000000

# Arena sizes in KB: doubling sequence covering "much too small" to
# "comfortably fits everything".
SIZES="1 2 4 8 16 32 64 128 256 512 1024 2048 4096"
POLICIES="none lru fifo"

echo "workload,policy,arena_kb,lx7_per_cyc,real_lx7_per_cyc,blocks,interp_fallbacks,smc_inv,resets"

extract() {
    awk '
        /^\[host\] blocks=/ {
            split($2, a, "[=/]"); blocks=a[2];
            for (i=1; i<=NF; i++) {
                if ($i ~ /^resets=/) { split($i,b,"="); resets=b[2]; }
            }
        }
        /^\[host\] interp_fallbacks=/ {
            split($2, a, "="); fallback=a[2];
        }
        /^\[BENCH\]/ {
            for (i=1; i<=NF; i++) {
                if ($i ~ /^lx7_per_cyc=/) { split($i,b,"="); lx=b[2]; }
                if ($i ~ /^real_lx7_per_cyc=/) { split($i,b,"="); rlx=b[2]; }
            }
        }
        END { printf "%s,%s,%s,%s,%s\n", lx, rlx, blocks, fallback, resets }
    '
}

for kb in $SIZES; do
    for policy in $POLICIES; do
        out=$($BIN --jit --evict "$policy" --arena-kb "$kb" \
                   --load-snapshot "$BENCH_SNAP" --max-cycles "$BENCH_CYCLES" 2>&1)
        # smc_invalidations isn't in the [host] line — get it from --profile
        # but the dispatcher doesn't print it; use 0 as placeholder
        row=$(echo "$out" | extract)
        echo "bench,$policy,$kb,$row,0"
    done
done

for kb in $SIZES; do
    for policy in $POLICIES; do
        out=$($BIN --jit --evict "$policy" --arena-kb "$kb" \
                   --rom "$ROM" --max-cycles "$BOOT_CYCLES" 2>&1)
        row=$(echo "$out" | extract)
        echo "boot,$policy,$kb,$row,0"
    done
done

# Interp baseline (one per workload). Format identically: lx7/cyc only.
echo "# interp baseline:"
out=$($BIN --interp --load-snapshot "$BENCH_SNAP" --max-cycles "$BENCH_CYCLES" 2>&1)
ilx=$(echo "$out" | awk '/^\[BENCH\]/ {for(i=1;i<=NF;i++) if($i~/^lx7_per_cyc=/){split($i,b,"=");print b[2]}}')
echo "bench,interp,0,$ilx,$ilx,0,0,0"

out=$($BIN --interp --rom "$ROM" --max-cycles "$BOOT_CYCLES" 2>&1)
ilx=$(echo "$out" | awk '/^\[BENCH\]/ {for(i=1;i<=NF;i++) if($i~/^lx7_per_cyc=/){split($i,b,"=");print b[2]}}')
echo "boot,interp,0,$ilx,$ilx,0,0,0"
