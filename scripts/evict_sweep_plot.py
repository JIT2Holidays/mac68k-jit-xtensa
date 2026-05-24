#!/usr/bin/env python3
"""Plot M6.63 eviction-policy sweep results.

Reads scripts/evict_sweep.csv, makes a two-panel figure (bench, boot)
showing lx7_per_cyc vs arena size for each policy (None / LRU / FIFO),
plus the interp baseline as a horizontal line.

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
        rows.append({
            "workload": parts[0],
            "policy":   parts[1],
            "kb":       int(parts[2]),
            "lx7":      float(parts[3]),
            "real":     float(parts[4]),
            "blocks":   int(parts[5]),
            "resets":   int(parts[8]) if len(parts) > 8 else 0,
        })

def series(workload, policy):
    pts = sorted([r for r in rows if r["workload"] == workload and r["policy"] == policy],
                 key=lambda r: r["kb"])
    return [r["kb"] for r in pts], [r["lx7"] for r in pts]

interp = {r["workload"]: r["lx7"] for r in rows if r["policy"] == "interp"}

def series_real(workload, policy):
    pts = sorted([r for r in rows if r["workload"] == workload and r["policy"] == policy],
                 key=lambda r: r["kb"])
    return [r["kb"] for r in pts], [r["real"] for r in pts]

fig, axes = plt.subplots(1, 2, figsize=(13, 5.5), sharey=False)
for ax, workload in zip(axes, ["bench", "boot"]):
    for policy, color, marker in [("none", "#d62728", "o"),
                                  ("lru",  "#2ca02c", "s"),
                                  ("fifo", "#1f77b4", "^")]:
        xs, ys = series_real(workload, policy)
        if xs:
            ax.plot(xs, ys, color=color, marker=marker, linewidth=2,
                    label=f"JIT {policy}", markersize=7)
    if workload in interp:
        ax.axhline(interp[workload], color="grey", linestyle="--", linewidth=1.5,
                   label=f"interp ({interp[workload]:.2f})")
    ax.set_xscale("log", base=2)
    sizes = sorted({r["kb"] for r in rows if r["workload"] == workload and r["kb"] > 0})
    ax.set_xticks(sizes)
    ax.set_xticklabels([f"{s}" if s < 1024 else f"{s//1024}M" for s in sizes],
                       rotation=45, ha="right")
    ax.set_xlabel("Arena size (KB, log2)")
    ax.set_ylabel("real lx7 / 68k cycle  (lower = faster)")
    ax.set_title(f"{workload}  —  JIT eviction policies vs arena size")
    ax.grid(True, alpha=0.3, linestyle=":")
    ax.legend(loc="best", framealpha=0.9)

fig.suptitle("M6.63 — JIT codecache eviction policies (mac68k-jit-xtensa)", y=1.00)
fig.tight_layout()
fig.savefig(OUT, dpi=140, bbox_inches="tight")
print(f"wrote {OUT}")
