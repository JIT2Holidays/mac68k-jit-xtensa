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

echo "workload,policy,arena_kb,lx7_per_cyc,real_lx7_per_cyc,blocks,interp_fallbacks,smc_inv,resets,elapsed_us"

extract() {
    awk '
        /^\[host\] blocks=/ {
            split($2, a, "[=/]"); blocks=a[2];
            for (i=1; i<=NF; i++) {
                if ($i ~ /^resets=/)   { split($i,b,"="); resets=b[2]; }
                if ($i ~ /^smc_inv=/)  { split($i,b,"="); smc=b[2]; }
            }
        }
        /^\[host\] interp_fallbacks=/ {
            split($2, a, "="); fallback=a[2];
        }
        /^\[host\] (JIT|interp) halted=/ {
            for (i=1; i<=NF; i++) {
                if ($i ~ /^elapsed=/) { sub(/us$/, "", $i); split($i,b,"="); us=b[2]; }
            }
        }
        /^\[BENCH\]/ {
            for (i=1; i<=NF; i++) {
                if ($i ~ /^lx7_per_cyc=/) { split($i,b,"="); lx=b[2]; }
                if ($i ~ /^real_lx7_per_cyc=/) { split($i,b,"="); rlx=b[2]; }
            }
        }
        END { printf "%s,%s,%s,%s,%s,%s,%s\n", lx, rlx, blocks, fallback, smc+0, resets, us }
    '
}

# Run each (workload, policy, size) point three times and average the
# wall-clock; xt_instrs / helpers are deterministic so we keep the
# first-run figures for those.
run3_avg() {
    # args: $1=workload (bench|boot), $2=policy, $3=arena_kb
    local w="$1" p="$2" k="$3"
    local first="" totals=""
    for r in 1 2 3; do
        if [ "$w" = "bench" ]; then
            out=$($BIN --jit --evict "$p" --arena-kb "$k" \
                       --load-snapshot "$BENCH_SNAP" --max-cycles "$BENCH_CYCLES" 2>&1)
        else
            out=$($BIN --jit --evict "$p" --arena-kb "$k" \
                       --rom "$ROM" --max-cycles "$BOOT_CYCLES" 2>&1)
        fi
        row=$(echo "$out" | extract)
        if [ -z "$first" ]; then first="$row"; fi
        us=$(echo "$row" | awk -F, '{print $NF}')
        totals="$totals $us"
    done
    # average elapsed_us across the three runs
    avg=$(echo $totals | awk '{s=0;for(i=1;i<=NF;i++)s+=$i; printf "%.0f", s/NF}')
    # rewrite the last field of `first` with the averaged us
    echo "$first" | awk -F, -v avg="$avg" 'BEGIN{OFS=","} {$NF=avg; print}'
}

for kb in $SIZES; do
    for policy in $POLICIES; do
        echo "bench,$policy,$kb,$(run3_avg bench $policy $kb)"
    done
done

for kb in $SIZES; do
    for policy in $POLICIES; do
        echo "boot,$policy,$kb,$(run3_avg boot $policy $kb)"
    done
done

# Interp baseline — also averaged, for the wall-clock plot.
interp_row() {
    local w="$1"
    local totals="" us=""
    for r in 1 2 3; do
        if [ "$w" = "bench" ]; then
            out=$($BIN --interp --load-snapshot "$BENCH_SNAP" --max-cycles "$BENCH_CYCLES" 2>&1)
        else
            out=$($BIN --interp --rom "$ROM" --max-cycles "$BOOT_CYCLES" 2>&1)
        fi
        ilx=$(echo "$out" | awk '/^\[BENCH\]/ {for(i=1;i<=NF;i++) if($i~/^lx7_per_cyc=/){split($i,b,"=");print b[2]}}')
        us=$(echo "$out" | awk '/^\[host\] interp halted=/ {for(i=1;i<=NF;i++) if($i~/^elapsed=/){sub(/us$/,"",$i);split($i,b,"=");print b[2]}}')
        totals="$totals $us"
    done
    avg=$(echo $totals | awk '{s=0;for(i=1;i<=NF;i++)s+=$i; printf "%.0f", s/NF}')
    echo "$w,interp,0,$ilx,$ilx,0,0,0,0,$avg"
}

echo "# interp baseline:"
interp_row bench
interp_row boot
