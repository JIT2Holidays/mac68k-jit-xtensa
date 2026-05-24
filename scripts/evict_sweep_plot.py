#!/usr/bin/env python3
"""Plot M6.63 eviction-policy sweep results.

Reads scripts/evict_sweep.csv, makes a 2x2 figure:
  top row: real_lx7_per_cyc — the JIT-execution cost metric (lower is better).
  bottom row: wall-clock us (3-run avg) — actual host time, which reveals
              allocator-overhead differences between policies that
              lx7_per_cyc misses.
  left column = bench, right column = boot.

Saves scripts/evict_sweep.png.
"""
import csv
from pathlib import Path
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

ROOT = Path(__file__).resolve().parents[1]
CSV  = ROOT / "scripts/evict_sweep.csv"
OUT  = ROOT / "scripts/evict_sweep.png"

rows = []
with CSV.open() as f:
    for line in f:
        line = line.strip()
        if not line or line.startswith("#") or line.startswith("workload,"):
            continue
        parts = line.split(",")
        if len(parts) < 10: continue
        rows.append({
            "workload": parts[0],
            "policy":   parts[1],
            "kb":       int(parts[2]),
            "lx7":      float(parts[3]),
            "real":     float(parts[4]),
            "blocks":   int(parts[5]),
            "fallback": int(parts[6]),
            "smc_inv":  int(parts[7]) if parts[7] else 0,
            "resets":   int(parts[8]) if parts[8] else 0,
            "us":       int(parts[9]) if parts[9] else 0,
        })

def series(workload, policy, field):
    pts = sorted([r for r in rows if r["workload"] == workload and r["policy"] == policy],
                 key=lambda r: r["kb"])
    return [r["kb"] for r in pts], [r[field] for r in pts]

interp_real = {r["workload"]: r["real"] for r in rows if r["policy"] == "interp"}
interp_us   = {r["workload"]: r["us"]   for r in rows if r["policy"] == "interp"}

fig, axes = plt.subplots(2, 2, figsize=(13, 8.5), sharex='col')

POLICY_STYLE = [("none", "#d62728", "o"),
                ("lru",  "#2ca02c", "s"),
                ("fifo", "#1f77b4", "^")]

def setup_xticks(ax, workload):
    sizes = sorted({r["kb"] for r in rows
                    if r["workload"] == workload and r["kb"] > 0})
    ax.set_xticks(sizes)
    ax.set_xticklabels([f"{s}" if s < 1024 else f"{s//1024}M" for s in sizes],
                       rotation=45, ha="right")
    ax.set_xscale("log", base=2)
    ax.grid(True, alpha=0.3, linestyle=":")

# Top row: real_lx7_per_cyc
for ax, workload in zip(axes[0], ["bench", "boot"]):
    for policy, color, marker in POLICY_STYLE:
        xs, ys = series(workload, policy, "real")
        if xs:
            ax.plot(xs, ys, color=color, marker=marker, linewidth=2,
                    label=f"JIT {policy}", markersize=7)
    if workload in interp_real:
        ax.axhline(interp_real[workload], color="grey", linestyle="--",
                   linewidth=1.5, label=f"interp ({interp_real[workload]:.2f})")
    setup_xticks(ax, workload)
    ax.set_ylabel("real lx7 / 68k cycle\n(JIT-execution cost, lower = faster)")
    ax.set_title(f"{workload}  —  lx7/cyc vs arena size")
    ax.legend(loc="best", framealpha=0.9)

# Bottom row: wall-clock us
for ax, workload in zip(axes[1], ["bench", "boot"]):
    for policy, color, marker in POLICY_STYLE:
        xs, ys = series(workload, policy, "us")
        ys_ms = [y / 1000.0 for y in ys]
        if xs:
            ax.plot(xs, ys_ms, color=color, marker=marker, linewidth=2,
                    label=f"JIT {policy}", markersize=7)
    if workload in interp_us:
        ax.axhline(interp_us[workload] / 1000.0, color="grey", linestyle="--",
                   linewidth=1.5,
                   label=f"interp ({interp_us[workload]/1000.0:.1f}ms)")
    setup_xticks(ax, workload)
    ax.set_xlabel("Arena size (KB, log2)")
    ax.set_ylabel("host wall-clock (ms, 3-run avg)\n(lower = faster, allocator cost included)")
    ax.set_title(f"{workload}  —  wall-clock vs arena size")
    ax.legend(loc="best", framealpha=0.9)

fig.suptitle("M6.63 — JIT codecache eviction policies: JIT-exec metric vs wall-clock\n"
             "(top: deterministic JIT work output; bottom: includes allocator overhead)",
             y=1.00)
fig.tight_layout()
fig.savefig(OUT, dpi=140, bbox_inches="tight")
print(f"wrote {OUT}")
