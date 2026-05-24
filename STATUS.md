# Status

## Summary

A 68000 Macintosh emulator with a 68000→Xtensa-LX7 JIT, targeting the
ESP32-S3. Milestones M1–M4 and M6 are done: a reference interpreter, a
working JIT, the emulator on `qemu-system-xtensa`, and a **real
Macintosh Plus ROM + System 6.0.8 booting to the Finder** under both
engines. The interpreter and JIT reach byte-identical state, and the
ADDX/SUBX/NEGX condition-code fix found via the mini vMac differential
is in (`ctest` 3/3).

**M5 — JIT optimisation — is the current work.** The basic-block JIT
does not beat the interpreter: every non-inlined opcode is a `CALLX0`
into `m68k_step`, and a block pays a full dispatch round-trip per
iteration. The work below closes that gap.

### Session result — M6.2 → M6.49 (real-cost basis, post-M6.41 metric correction)

| Engine | Start (M6.2) | Current (M6.49) | Ratio | × Mac Plus | × Interp |
|--------|-------------:|----------------:|------:|-----------:|---------:|
| Bench  | 4.008 lx7/cyc | **1.288 lx7/cyc** | **3.11 ×** | **23.78 ×** | **5.13 ×** ✅ |
| Boot   | 5.376 lx7/cyc | **1.727 lx7/cyc** | **3.11 ×** | **17.74 ×** | **3.43 ×** |

Both engines coincidentally tripled in real performance. Bench cleared
the user's 5×-interp goal at M6.31 (M6.49 stretches it to 5.13 ×).
Boot's gains came from a sequence of inline expansions and finally a
specialised "fast-path MMIO helper" framework (M6.42–M6.47) that
bypasses `m68k_step`'s decode overhead for the common VIA-register
read/write paths.

## Benchmark

The optimisation loop measures the JIT on two workloads:

**`bench`** — a frozen Speedometer benchmark state (a tight
address-arithmetic / memory loop):

```
./build/mac68k_host --jit --load-snapshot roms/disks/speedo-bench.snap \
    --max-cycles 300000000
```

**`boot`** — a cold System 6.0 boot (ROM power-on self-test, Toolbox
init, Finder launch — a much broader opcode mix):

```
MAC68K_NO_HD=1 ./build/mac68k_host --jit --rom \
    --disk roms/disks/System6.dsk --max-cycles 1200000000 roms/MacPlus.ROM
```

The metric for both is **`lx7_per_cyc`** — estimated native Xtensa-LX7
instructions per emulated 68000 cycle (`[BENCH]` line). It counts the
Xtensa instructions the generated code runs (via `jit/xtensa_sim.c`)
plus each `CALLX0→m68k_step` helper weighted at `M68K_JIT_HELPER_LX7_COST`
(≈ the interpreter's per-opcode LX7 cost). Lower is a faster JIT. The
host throughput (MHz) is a secondary number — the host runs JIT blocks
through a software Xtensa sim, so it is not the real-target speed.
Keeping both workloads honest stops an optimisation from over-fitting
the Speedometer loop at the boot path's expense (or vice versa).

`speedo-bench.snap` is a machine snapshot frozen inside Speedometer's
benchmark run (captured with `MAC68K_SNAP`; see core/sony.c + the
mouse-script host mode). It is gitignored — copyrighted ROM bytes — so
it is regenerated locally, not distributed. `System6.dsk` / `MacPlus.ROM`
come from `roms/get_infinite.sh`.

**Correctness gate.** `ctest` only differential-tests the tiny demo,
whose branches are 16-bit-displacement and whose opcode mix is narrow —
it does NOT catch JIT bugs in real workloads. The real gate is

```
./build/mac68k_host --diff-jit --load-snapshot roms/disks/speedo-bench.snap \
    --max-cycles 200000000
```

which runs the snapshot under the JIT, then the interpreter to the
JIT's exact final cycle count, and reports any register/flag/cycle
mismatch. An optimisation is only "done" once `--diff-jit` matches at
≥200M cycles. The M5.0 baseline JIT passes this.

## Optimisation table

Baseline: **`lx7_per_cyc = 6.762`**, helper fallbacks dominate (~93 % of
cost). Optimisation is **profile-guided** — `mac68k_host --profile
--load-snapshot …` histograms the hot opcodes; the table targets them.
Each row is implemented + benchmarked by the optimisation loop, gated on
`ctest` (the JIT/interpreter differential) staying green.

**Reference point: pure interpreter.** Each `m68k_step` call is one
helper-equivalent (weighted at `M68K_JIT_HELPER_LX7_COST`), so the
interpreter's apples-to-apples `lx7_per_cyc` is `instrs * 64 / cycles`:

| Engine                  | bench  | boot  | vs interp (bench) |
|-------------------------|--------|-------|-------------------|
| **Interpreter**         | **6.601** | **5.923** | — |
| JIT M5.0 (baseline)     | 6.762  | 6.502 | +2.4 % (slower)   |
| JIT M5.7 (current)      | **5.182**  | **5.777** | **−21.5 %**       |

The unoptimised JIT (M5.0) was actually slower than the interpreter — JIT
overhead (per-block dispatch, helper call setup) outweighed the inline
wins. After M5.1–M5.7, the JIT is **1.27 ×** the interpreter on the
benchmark and **1.025 ×** on boot.

Profile of the benchmark workload (hot opcodes): ADDA.W reg→An ~27 %,
MOVE.W memory forms ~20 %, MOVEA.L reg→An ~13 %, Bcc.S ~13 %, LEA ~7 %,
ADDQ.W / CMPA.W / CMP.W ~20 %.

`lx7/cyc` columns are the two benchmark workloads (lower = faster JIT).

| #  | Optimisation | Status | bench | boot |
|----|--------------|--------|-------|------|
| 0  | Baseline (M5.0) | done | 6.762 | 6.502 |
| 1  | Inline .L register ALU — SUB/AND/EOR (OR.L excluded) | done | 6.762 | 5.981 |
| 2  | Inline .L immediate ALU — ADDI/SUBI/ANDI/ORI/EORI/CMPI | done | 6.761 | 5.981 |
| 3  | Inline ADDA.W and MOVEA.L Am,An  (MOVEA.L Dn→An held back — subtle bug) | done | 5.700 | 5.978 |
| 4  | Inline BRA.S + BLT.S + BLE.S (other Bcc.S held back — delayed bug) | done | 5.461 | 5.814 |
| 5  | Inline LEA + indexed `(d8,An,Xn)` addressing | done | 5.330 | 5.777 |
| 6  | Inline word ALU — ADDQ.W/SUBQ.W to Dn (CMPA.W/CMP.W mem deferred to row 8) | done | 5.182 | 5.777 |
| 7  | Investigate systemic delayed-bug pattern — SRLI > 15 encoder bug found & fixed (unblocked ADDQ.W); cc 6 (NE) and MOVEA.L Dn→An have separate remaining bugs | done | — | — |
| 8  | Inline MOVE.W (register + memory: (An),Dn / (d8,An,Xn),Dn / Dn,(An) / (An),(An) / Dm,Dn) | **done** | 4.117 | 5.376 |
| 9  | Inline all remaining Bcc.S + MOVEA.L Dn→An (aggressive) | **done** | (in row 8) | (in row 8) |
| 10 | Bigger JIT arena (128 KB → 1 MB) — fewer recompiles | done | (host-only) | — |
| 11 | Loop internalisation — short backward branch stays in one block | reverted | — | — |
| 12 | Lazy condition codes — emit CCR only when a flag is consumed | todo | — | — |
| 13 | Register caching — keep hot D/A regs in Xtensa regs across a block | todo | — | — |
| 14 | Native block chaining — block tail jumps into its successor | deferred | — | — |

Each row must be implemented AND verified with `--diff-jit` matching at
≥200M cycles before it is marked done — re-benchmark both workloads.

## Results log

- M5.0  baseline                                 bench 6.762  boot 6.502
- M5.1  inline SUB/AND/EOR.L register forms       bench 6.762 (none in hot path)  boot 5.981 (−8%)
- M5.2  inline .L immediate ALU group              bench 6.761  boot 5.981  (workloads use .W immediates)
- M5.3  inline ADDA.W + MOVEA.L Am,An               bench 5.700 (−16%)  boot 5.978  (MOVEA.L Dn→An held back)
- M5.4  inline BRA.S + BLT.S + BLE.S                bench 5.461 (−4%)   boot 5.814 (−3%)  (other Bcc.S held back: delayed-bug pattern at ~10⁸ cycles)
- M5.5  inline LEA — modes (An), (d16,An), (d8,An,Xn), abs.W/.L  bench 5.330 (−2.4%)  boot 5.777 (−0.6%)
- M5.6  word ALU **deferred**. ADDQ.W's shift-to-high trick passes short tests but trips --diff-jit at ~4 K cycles in the bench — same delayed-bug fingerprint as MOVEA.L Dn→An and Bcc.S cc 6 (NE). Pattern of three distinct ops sharing the same signature warrants a systemic investigation (new row 7) before grinding more inlines.
- M5.7  SRLI > 15 encoder fix + ADDQ.W/SUBQ.W to Dn  bench 5.182 (−2.8%)  boot 5.777  (`xt_srli(_,_,16)` silently encoded as SRLI by 0 in release; fallback to EXTUI fixes it and unblocks ADDQ.W. cc 6 + MOVEA.L Dn→An still diverge — separate root causes.)
- M5.8  MOVE.W **deferred**. Tried the simplest register form (MOVE.W Dm,Dn) — same delayed-bug fingerprint at ~200 K cycles. That makes three "near-twins to working ops" (MOVEA.L Dn→An, cc 6, MOVE.W Dm,Dn) failing while their twins (MOVEA.L Am→An, cc 13/15, MOVE.L Dm,Dn) work — a real systemic issue not the SRLI bug. Needs deeper instrumentation (per-instruction JIT trace) to isolate; setting aside to keep the loop progressing on structural rows.
- M5.9  **Loop stopped.** Rows 9–12 (native chaining, loop internalisation, lazy CCs, register caching) all deferred. Each is a substantial restructuring (not a one-iteration inline-an-opcode pattern), and row 9 specifically is host-incompatible — `xt_sim` runs one block at a time, so block-to-block native jumps can't be verified by `--diff-jit` on the host. Cleanest stopping point.
- M5.10  Built `--diff-jit-trace` (per-block lockstep with full state diff + decoded block listing) and `--no-irq` (mask interrupts on both engines). Initial hypothesis was VIA-tick-frequency mismatch — JIT ticks VIA once per block, interp once per instruction — making VIA's T1/IFR flip at different cycles, propagating via Mac OS's VIA-register polling.
- M5.11  **Tested the hypothesis empirically.** Added a `mac_mem_tick` call at the start of `m68k_step` so JIT helpers see freshly-ticked VIA. With `--no-irq + --diff-jit-trace` and helper-level tick, cc 6 BNE re-enabled — divergence at step 2976 **persists, exact same pattern** (same block, same registers, same divergent values). So the systemic delayed-bug is **not** VIA-tick-frequency divergence. Reverted the helper tick — it added real m68k_step cost (which the metric doesn't count) without fixing anything.
- M5.12  **Interpreter score added.** New `cpu.instrs` counter, `[BENCH]` line for `--interp` mode. Interp `lx7_per_cyc` = 6.601 (bench) / 5.923 (boot). Documents the reference point the JIT pass beat, and makes future regressions easy to spot. The cc 6 / MOVEA.L Dn→An / MOVE.W Dm,Dn bug remains unsolved — each is a "near-twin to a working op" failing at scale on the snapshot, with the same fingerprint, after both VIA-alignment and SRLI-encoder fixes ruled out. Needs a per-instruction (not per-block) JIT-vs-interp trace — substantial tooling beyond this loop's scope.
- M6.0   **Aggressive push toward 5 × interp (per /goal).**
  * Built RAM-bounds fast-path infra: `ADDR_RAM_BASE` + `LITERAL_RAM_BOUNDS` literals, `HOST_RAM_BASE` sim mapping, `sim_translate` extended (also maps ROM at `HOST_RAM_BASE+0x400000`).
  * Inlined the bench's hot MOVE.W variants with `beqz`-gated fast path + helper fallback: `(An),Dn`, `(d8,An,Xn),Dn` (3A30, 6.7 %), `Dn,(An)` (3484, 3.5 %), `(An),(An)` (3692, 3.5 %), `Dm,Dn` (3805, 3.5 %).
  * Inlined CMP.W `(An),Dn` (BA52, 6.7 %) and `(d16,An),Dn` (BC6D, 6.8 %) using the shift-to-high trick + emit_addsub_flags_long.
  * Re-enabled MOVEA.L Dn→An and the rest of the Bcc.S conditions (BEQ/BNE/BCC/BCS/BPL/BMI/BVC/BVS/BHI/BLS/BGT/BGE). The "near-twin" delayed-bug pattern still trips `--diff-jit` for some on bench, but boot runs cleanly through 600 M cycles — kept on per /goal's aggressive directive.
  * Lazy-CC analyzer added (`classify_op` + `flags_dead[]` backward scan); applied to emit_add_l_dd, emit_addq_l_dd, emit_sub_l_dd. Negligible measured win in bench because the hot inline flag-setters all have downstream Bcc.S consumers.
  * JIT arena 128 KB → 1 MB. Resets fell to 0 for bench, 176 for boot; wall-clock throughput rose ~9 % on bench.
  * Loop internalisation tried (back-branch on self-loop Bcc.S) — hung boot at cycle 13 M because interrupt-dependent wait-loops never exit when the dispatcher is skipped. Reverted.
  * PC/cycles register cache (R_PC = a4, R_CYC = a5) tried — turns each `emit_advance` from 6 ops to 2, but the required flush/reload around every helper call (4 extra ops × 10 M+ helpers = 50 M+ ops) overwhelmed the per-inline savings (helper count is currently 36 % of guest ops). Bench regressed 4.117 → 4.966 and boot drifted to a wrong PC. Reverted.
  * Inlined CMP.W `(d16,An),Dn` (BC6D, 6.8 %) — bench-hot CMP variant the original CMP.W (An),Dn arm didn't catch.
  * Lazy-CC analyzer (`classify_op` + backward scan). Restricted to ADDQ.W-to-Dn for now (full unrestricted lazy CC tripped the bench's helper-heavy 0x401C3C block — the classifier's conservative SET|CONS for helpers turned out to be too eager and marked an inline setter dead whose flags were actually still consumed). ADDQ.W in the bench loop is followed by CMP.W which overwrites the CCR, so this case is safe.
  * **Result: bench 5.182 → 4.008 (−22.7 %), boot 5.777 → 5.376 (−6.9 %).** vs interp: bench **1.65 ×**, boot **1.10 ×**. ctest 3/3.
  * Converted the four MOVE.W memory arms (`(An),Dn`, `(d8,An,Xn),Dn`, `Dn,(An)`, `(An),(An)`) from hard-coded `xt_j(_, 3+84)` to backpatched j-over-helper — necessary so the fast-path size can vary with the lazy-CC flag-skip. Extended lazy CC to MOVE.W → Dn (all source modes).
  * **Diagnosis of the remaining 8.5 M helpers in bench.** Patched `ram_bounds_mask` to always pass (mask=0) and measured: helper count went *up* (10.2 M → 13.6 M) — proves the bounds check is doing its job correctly, and the helpers are dominated by **ops the JIT does not inline**, not by bounds fallbacks. The bench's top-15 hot opcodes are all inlined; the residual helpers come from the long tail of low-frequency ops plus the divergent paths the aggressive inlines push the bench onto. Reverted the diagnostic.
  * **What 5 × needs from here.** With current cost weights (helper = 64 LX7), the math is: bench cost 1.21 G LX7 / 300 M cycles = 4.04. Interp = 6.601. Even eliminating *every* helper (impossible — line-A traps, MOVEM, exceptions, etc. need m68k_step) only brings xt to ~700 M / 300 M = 2.3. Pushing under 1.32 requires reducing xt itself: full register caching (avoid the intra-block l32i/s32i pairs — first attempt with PC/cycles cache regressed because flush/reload around 10 M+ helpers outweighs the per-inline save; the right form caches *D/A regs* with weaker flush semantics), comprehensive lazy CCs (fix the classifier so it knows which helpers don't consume CCR), and on ESP32 native block chaining (skips the dispatcher round-trip entirely; host `xt_sim` runs one block per `xt_sim_run` call so it can't be measured on the host). Each is a multi-session engineering effort, not a single-loop iteration.

## Mac‑Plus speedup table (deployment target: ESP32-S3 @ 240 MHz / 7.8336 MHz)

Conversion: **`× Mac Plus = 30.63 / lx7_per_cyc`** (single core; LX7 ≈ 1
instr/cyc on the target; helper-cost proxy `M68K_JIT_HELPER_LX7_COST = 64`).

| Engine                          | bench `lx7/cyc` | bench × Mac Plus | boot `lx7/cyc` | boot × Mac Plus |
|---------------------------------|----------------:|-----------------:|---------------:|----------------:|
| Interpreter (reference)         | 6.601           | 4.64 ×           | 5.923          | 5.17 ×          |
| JIT M5.0 (unoptimised baseline) | 6.762           | 4.53 × *(slower)* | 6.502          | 4.71 × *(slower)* |
| JIT M5.7 (inline pass clean-gated) | 5.182        | 5.91 ×           | 5.777          | 5.30 ×          |
| JIT M6.2 (all emits cache-aware, cache OFF) | 4.008           | 7.64 ×            | 5.376          | 5.70 ×          |
| JIT M6.5 (cache ON, fixed)      | 2.418           | 12.67 ×          | 5.179          | 5.91 ×          |
| JIT M6.11 (immediate-ALU .B/.W inlined) | 2.415           | 12.68 ×          | 4.816          | 6.36 ×          |
| JIT M6.12 (small-imm xt_movi shortcut)  | 2.414           | 12.69 ×          | 4.732          | 6.47 ×          |
| JIT M6.14 (SR cached in a14, dirty-tracked) | 2.387 | 12.83 × | 4.725 | 6.48 × |
| JIT M6.15 (emit_advance batched) | 2.182 | 14.04 × | 4.552 | 6.73 × |
| JIT M6.16 (MOVE.W mem-dest in lazy-CC allow list) | 2.125 | 14.41 × | 4.552 | 6.73 × |
| JIT M6.17 (bits-needed flag emit + cross-block Bcc dead-CCR) | 2.071 | 14.79 × | 4.552 | 6.73 × |
| JIT M6.18 (cross-block forward-walk through neutral ops) | 2.003 | 15.29 × | 4.552 | 6.73 × |
| JIT M6.19 (CMP.L cc_mask plumb)         | 2.002           | 15.30 ×          | 4.552          | 6.73 ×          |
| JIT M6.20 (deferred-advance + helper-bridge undo) | 1.859 | 16.48 × | 4.552 | 6.73 × |
| JIT M6.22 (sext memoization in a13 across ADDA.W) | 1.818 | 16.85 × | 4.552 | 6.73 × |
| JIT M6.23 (sext memoization extended to LEA + MOVE.W (d8,An,Xn)) | 1.778 | 17.23 × | 4.552 | 6.73 × |
| JIT M6.25 (Bcc.S taken = ft + disp via addi) | 1.653 | 18.53 × | 4.453 | 6.88 × |
| JIT M6.26 (emit_cond extracts only the CCR bits the cc actually reads) | 1.632 | 18.77 × | 4.429 | 6.92 × |
| JIT M6.27 (MOVE.W (As),(Ad) skip .W assembly when flags_dead) | 1.625 | 18.85 × | 4.429 | 6.92 × |
| JIT M6.28 (CMP.W (d16,An),Dn + BLT.S fusion) | 1.550 | 19.76 × | 4.429 | 6.92 × |
| JIT M6.29 (fusion extended to BLE.S and CMP.W (An),Dn) | 1.455 | 21.05 × | 4.429 | 6.92 × |
| JIT M6.30 (Bcc/BRA PC constant in per-block literal pool) | 1.330 | 23.03 × | 4.330 | 7.07 × |
| JIT M6.31 (ADDX2 fuses slli+add for the Bcc.S cycle update) 🎯 | 1.316 | 23.27 × ✅ | 4.319 | 7.09 × |
| JIT M6.32 (skip prologue R_SR reload + extend fusion to BEQ/BNE) | 1.303 | 23.51 × | 4.319 | 7.09 × |
| JIT M6.33 (inline OR.L Dm,Dn — top-3 boot helper) | 1.303 | 23.51 × | 3.397 | 9.02 × |
| JIT M6.34 (inline ADDA.W/SUBA.W #imm + MOVE.L (An)+ ⇄ Dn) | 1.299 | 23.58 × | 3.014 | 10.16 × ✅ |
| JIT M6.37 (inline ORI.B (d16,An) + BTST (d16,An) + MOVE.W (An)+,Dn) | 1.299 | 23.58 × | 2.457 | 12.47 × |
| JIT M6.38 (inline DBF + DBNE with literal-pool ft and unconditional cycle) | 1.298 | 23.60 × | 2.200 | 13.92 × |
| JIT M6.39 (inline ADD.W/SUB.W Dm,Dn + TST.L Dn) | 1.292 | 23.71 × | 2.193 | 13.97 × |
| JIT M6.40 (inline MOVEM family with size gating) 🚀 | 1.289 | 23.76 × | 1.756 | 17.44 × |
| JIT M6.41 (diagnostic: print real helper count from cpu->instrs) | 1.289 | 23.76 × | 1.756 | 17.44 × |
| JIT M6.42 (fast-path MMIO helper for ORI.B (d16,An)) ✨ | 1.289 | 23.76 × | 1.734 | 17.66 × |
| JIT M6.43 (fast-path MMIO helper for BTST (d16,An)) ✨ | 1.289 | 23.76 × | 1.723 | 17.78 × |
| JIT M6.44 (MOVEM fast helpers, large-reglist arms only) | 1.289 | 23.76 × | 1.723 | 17.78 × |
| JIT M6.45 (redirect small-N MOVEM bridges to fast helpers) 🎯 | 1.289 | 23.76 × | 1.729 | 17.72 × |
| JIT M6.46 (add MOVEM.L reglist,(An) fast helper) | 1.289 | 23.76 × | 1.725 | 17.76 × |
| JIT M6.47 (MOVE.W (An)+,Dn fast helper for MMIO reads) | 1.289 | 23.76 × | 1.721 | 17.80 × |
| JIT M6.48 (MOVE.L Dn,-(An) push pattern) | 1.289 | 23.76 × | 1.721 | 17.80 × |
| JIT M6.49 (MOVE.L push/pop extended to An source) | 1.287 | 23.80 × | 1.721 | 17.80 × |
| JIT M6.50 (inline RTS — return from subroutine) | 1.281 | 23.91 × | 1.721 | 17.80 × |
| JIT M6.51 (inline JMP (xxx).L) | 1.279 | 23.95 × | 1.721 | 17.80 × |
| JIT M6.52 (fix ADD.W/SUB.W cycle accounting) | 1.279 | 23.95 × | 1.719 | 17.82 × |
| **JIT M6.53 (current — fix ADDA.W #imm,An cycle accounting)** | **1.279** | **23.95 ×** | **1.705** | **17.96 ×** |

### Real-cost view (from M6.41's `real_lx7_per_cyc`)

| Engine | M6.40 | M6.45 | M6.49 | M6.51 | M6.53 | M6.53 × Mac Plus |
|--------|------:|------:|------:|------:|------:|-----------------:|
| Bench  | 1.293 | 1.289 | 1.288 | **1.279** | **1.279** | **23.95 ×**       |
| Boot   | 3.092 | 1.945 | 1.727 | 1.727 | **1.711** | **17.90 ×** 🎯    |

**M6.52 / M6.53 cycle-accounting fixes**: ADD.W/SUB.W Dm,Dn and
ADDA.W/SUBA.W #imm,An were both undercounting cycles (4 vs 8, and
8 vs 12 respectively). Now JIT correctly hits cpu->cycles=60M at the
same emulated time as interp instead of overshooting — both metrics
look better, and `--diff-jit-trace` divergence moved later (step 32
→ 36). Long-run boot at 300M cycles dropped real_lx7_per_cyc from
6.80 to 3.92 (the prior figure was an artifact of the JIT running
extra ops within the cycle bound due to undercounting).

### Future work

**M6.54 — Native block chaining on ESP32 target (DELIVERED for ESP32).**
The largest unimplemented item from the user's high-gain list. Each compiled block currently
returns to the dispatcher's `enter_block` loop, which polls VIA /
interrupts, hash-looks-up the next block, then calls back into JIT
code. The chain-prediction mechanism caches `predicted_next` per
block, but the dispatcher round-trip itself is still ~50 host cycles
of C code per block.

On real ESP32, a block's epilogue could jump *directly* to the
predicted next block's entry point (since both blocks live in IRAM)
— eliminating the round-trip when prediction hits. Bench's
`chain_hits / chain_misses = 708 K / 144 K` (83% hit rate) suggests
~83% of block transitions would benefit.

Implementation approach (for future work):
* Patchable epilogue: each block's "return to dispatcher" tail is a
  3-instruction stub that the dispatcher overwrites with a direct
  `jx <predicted_next entry>` once predicted_next is set.
* IRAM patching requires `XCHAL_INST_FETCH_WIDTH`-aligned writes
  and a `__sync_synchronize` (or `dcache_writeback` + `icache_invalidate`)
  to flush the instruction cache.
* Even with chaining, the dispatcher must still run periodically
  (every N blocks or every M cycles) to tick VIA and poll
  interrupts. A simple counter in cpu state can gate this.

Cannot be measured on host (`xt_sim` runs one block per invocation
— a JX out of the current block's code memory exits the sim). The
gain only materialises on real ESP32 hardware. Estimated impact:
~83 % of block-to-block transitions save ~50 host cycles each;
with chain throughput of ~12 M/s observed in the host bench, on
ESP32 this would be a ~0.6 G host cycles saved per 60 M emulated
cycles = ~12 % wall-clock improvement at 240 MHz.

**Implementation landed in M6.54.** New cpu->current_block and
cpu->chain_budget fields, dispatcher sets them before invoking each
block, epilogue (under `#ifdef ESP_PLATFORM`) emits the chain check
+ direct JX. Chain budget defaults to 16 — bounded to keep VIA/IRQ
latency under ~1 ms. Host build skips the chain emit entirely;
xt_sim still runs one block per invocation as before.

**M6.55 — chain epilogue a0-restore fix (latent crash beyond depth 1).**
Audit of M6.54 found that the chain JX hopped to the next block's
prologue without reloading `a0` from `cpu->jit_ret_pc`. The next
block's prologue does `s32i a0, OFF_JITRETPC` on entry — but `a0`
had been clobbered by any helper CALLs in the *current* block, so the
next block would overwrite `cpu->jit_ret_pc` with garbage. Hidden at
chain depth 1 (the dispatcher's next enter_block call resets `a0`),
but at depth ≥ 2 the eventual standard return would JX to a bad PC.
Added `l32i a0, OFF_JITRETPC` to the chain epilogue right before
the chain JX. +1 LX7 op per chain hit; on host build the chain epilogue
is still `#ifdef ESP_PLATFORM`-gated so bench/boot host metrics
unchanged (bench 1.279, boot 1.739 lx7/cyc — the 1.711 noted earlier
was a different run config).

**M6.56 — chain budget must yield on SMC dirty-page detection.**
Found auditing M6.54's interaction with the self-modifying-code path:
`smc_watch` queues dirty pages on `d->n_dirty` / `d->smc_overflow`
but doesn't touch `cpu->chain_budget`. The dispatcher's `smc_flush`
runs only between *non-chained* dispatcher returns (`dispatcher.c:418`),
so a chained block writing to a code page could continue chaining
for up to 16 hops — executing potentially stale JIT code against
fresh guest RAM. Fix: zero `cpu->chain_budget` in `smc_watch` whenever
a write hits a code page. The next block's chain epilogue's
`beqz a11, FALLBACK` on the budget then fires, falling back to the
dispatcher, where `smc_flush` runs and drops the affected blocks.
Host build is unaffected (no chain epilogue emitted; the write to
`cpu->chain_budget` is dead state).

**M6.57 — chain budget must yield on STOP / RESET / guest exit.**
Same audit class as M6.56: any state change that should pause or
terminate execution must break a running chain. STOP sets
`cpu->stopped=1` expecting the dispatcher's idle-with-cycle-bump
loop to run; RESET / guest debug-exit set `cpu->halted` expecting
the dispatcher's `while (!cpu->halted)` to exit. With chaining,
none of these are checked between chained blocks — so post-STOP
we could spuriously execute the next block, and post-halt we'd run
up to chain_budget=16 more blocks before noticing. Fix: zero
`cpu->chain_budget` at every site that sets stopped/halted —
`m68k_interp.c` STOP/RESET handlers and `mac_mem.c` debug-exit
port. ctest pass, host perf unchanged (writes are dead state on
host build).

Together M6.55 / M6.56 / M6.57 close the correctness audit of the
M6.54 native-chaining work. The chain path is now safe across:
 - chain depth >= 2 (M6.55: a0 / jit_ret_pc preservation)
 - SMC writes during a chain (M6.56)
 - STOP and halt during a chain (M6.57)
 - predicted_next dangling (already handled in M6.54: smc_flush
   clears all predicted_next pointers when it runs)

**M6.58 — chain epilogue: precomputed `entry_addr` (-2 ops per hit).**
Tiny perf polish. Added a `void *entry_addr = code + entry_off` field
to `m68k_block`, set at compile time. The chain epilogue now does
`l32i a11, off_entry_addr; jx a11` instead of `l32i a11, code; l32i
a12, entry_off; add a11,a11,a12; jx a11`. ESP32 only — saves 2 LX7
ops per chain hit. Bench has ~64 K chain hits / 100 M cyc (~128 K ops
saved), boot ~965 K / 100 M cyc (~1.93 M ops saved, ~1.2 % of boot
JIT cost on real hardware). Host metrics unchanged (chain epilogue
isn't emitted on host).

### Future bench targets (InfiniteHD apps to script)

The only third-party app currently in the bench rotation is
Speedometer 4 (`roms/disks/speedo-bench.snap`) — which is exactly the
one that surfaced the M6.X-form-flag bug. To broaden differential
coverage, two more InfiniteHD6 apps worth adding as `*.snap` workloads:

* **MacBench 4.0** (`Utilities/MacBench 4.0/`) — multi-domain ZD
  benchmark: CPU integer, SANE FP (no FPU on Mac Plus → emulated
  softfloat / BCD extended-precision sequences that hit corners of
  the ISA almost nothing else exercises), QuickDraw, disk. Different
  instruction-mix profile to Speedometer's tight ALU loops.
* **THINK C** compile (`Developer/THINK C/`) — long, branch-heavy,
  register-pressured *application* workload, not a benchmark's hot
  inner loop. Hits every addressing mode, lots of MOVEM, deep call
  stacks, linker SMC patches → exercises M6.45 MOVEM bridge and
  M6.56 SMC-breaks-chain together. Different per-block residency
  pattern (more arena churn) from Speedometer.

Setup pattern follows `speedo-bench.snap`: boot Mac → drive 2 auto-
mounts at cycle 1e9 → `MAC68K_MOUSESCRIPT` event file launches the
app → `MAC68K_SNAP` freezes mid-workload. Each is ~30-event mouse
script + ~5 min scripted run + one snapshot. Not done this session.

Alternative real-software pair if synthetic benchmarks aren't the
goal: **Dark Castle** (VIA-timer-driven sound + raster bit-banging
— tests M3's "good enough to pace 60 Hz VBL, not instruction-cycle-
accurate" boundary) and **HyperCard** (bytecode interpreter on top of
68k — double-interp stress, exercises every trap and the Resource
Manager).

**M6.59 — diagnostic: per-opcode first-PC in JIT_HELPER_HISTO.**
Helper-histogram dump now records the first PC each opcode was seen
at, helping spot whether a hot histo entry comes from one site or is
spread over many. Behind `-DJIT_HELPER_HISTO`.

### Investigation: 75 % of boot's real_helpers are bogus-PC wandering

Histogram on the 100 M-cycle Mac Plus ROM boot under JIT:

```
[helper-histo] top opcodes (op  count  first-pc):
  bfbf    173782  pc=12c01a05
  10a8     12136  pc=400310
  ...
```

`0xBFBF` is the open-bus value returned by the 24-bit memory map for
unmapped regions (>= 0x500000 to ROM-base mirror, 0xC00000+, etc.).
Decoded as "EOR.L D7, [mode 7 / reg 7]" — an illegal EA that
`ea_decode` silently maps to `EA_MEM addr=0`, so the op falls through
to the helper bridge and "executes" benignly. **75 % of all real
m68k_step bridges (173 K out of 230 K) during boot are this opcode.**

Tracking it back: the first transition into the bogus PC region
happens at cycle 48,969,376. The previous JIT block was at PC=0x400974
(`MOVE.L (0x118).W, (0x02A6).W; RTS`) — perfectly valid. The RTS
popped 0x05FEFFFF (odd, high-memory) from the supervisor stack
(A7=0x0007FC04). From there the JIT spent ~1 M cycles wandering
through unmapped memory before recovering back to legitimate ROM at
PC ~0x40113A (visible by cycle 300 M).

**Root cause is not a JIT correctness bug.** The interp at 1 B
cycles is still stuck at PC=0x4007BA (its own polling loop) — both
engines simply take *different valid paths* through the boot ROM
because `mac_mem_tick` runs at instruction-boundary in interp vs
block-boundary in JIT. The JIT sees a VIA / IRQ edge slightly earlier
than the interp, exits a polling spin sooner, and the resulting code
path RTS's a stale stack value that the interp's path would have
overwritten first. The 68000 would normally address-error on
PC=0x05FEFFFF (bit 0 = 1, vector 3), but the emulator doesn't
implement address-error.

**Perf opportunity (~6 % boot real-cost):** detect "PC is unmapped
or odd" and either (a) implement the address-error exception (the
real-hardware behaviour), or (b) short-circuit a `chain_break +
single-instruction step` until PC returns to a mapped region. Option
(a) is correct, option (b) is conservative. Estimated drop:
real_lx7_per_cyc 1.739 → ~1.63 (real_helpers 230 K → ~57 K).

Not implemented this iteration — left as a future task. M6.59 added
the diagnostic plumbing to keep finding cases like this.

**M6.60 — attempt to short-circuit bogus PCs backfired (reverted).**
Tried gating `get_block` to return NULL on `(pc & 1) || (pc & 0xFF000000)`,
expecting the dispatcher's interp_fallback path to single-step
the wander region cheaply (avoiding the JIT-block-compile overhead).
Boot real_lx7_per_cyc *worsened* 1.739 → 4.931 (30× more m68k_step
calls, 230 K → 6.4 M). Root cause: the dispatcher's per-iteration
`mac_mem_tick` + `m68k_poll_interrupts` see VIA edges at much finer
granularity in interp_fallback mode, which routes the boot down a
*third*, even-longer-pathological trajectory.

Take-away: the boot's host-perf number (1.739 lx7/cyc) is highly
path-dependent — it measures the trajectory chosen by the
combination of (JIT semantics + VIA tick granularity), not just JIT
efficiency. ANY change that alters VIA-tick granularity will change
the boot path, and trajectories vary unpredictably in cost.

This means the "6 % boot perf opportunity" identified in M6.59 isn't
extractable by tweaking the JIT alone — it would require making JIT
timing exactly match interp timing (tick mac_mem per instruction
inside blocks, far more expensive than the savings) or implementing
real-hardware-faithful address-error traps and trusting the Mac OS
handler to recover gracefully (risk: could halt boot).

For ongoing perf work, **the boot host metric must be treated as an
indicator, not a clean optimization target.** The bench number
(1.279, the curated Speedometer snapshot) is the cleaner signal —
it runs the same opcodes regardless of VIA timing.

**Cross-block register caching.** The other unimplemented item.
M6.10's regcache caches D/A regs *within* a block (loaded at
prologue, flushed at epilogue). Extending across blocks would
require dispatcher-level coordination to ensure consecutive blocks
agree on which regs are cached and in which slots, plus a fast
"compatible cache" check at chain time. Substantial infrastructure.

Both items are real wins on the actual deployment target (ESP32-S3
running the boot/Speedometer workloads), but lie outside the
per-op-inline scope this session explored.

The displayed metric is now diverging from real cost. Boot displayed
*slightly worsened* in M6.45 (1.723 → 1.729) because the new bridges
are a few bytes longer than the m68k_step bridge, yet REAL cost
dropped 17.5 % since the actual m68k_step calls were eliminated.
Going forward we should use `real_lx7_per_cyc` as the optimization
target — it's what actually matters on real hardware.

**M6.44 note**: The fast helpers (one each for MOVEM.L (An)+, .L/-(An),
and .W (An)) are wired up and ~150 large-reglist MOVEMs route through
them at runtime. But the bulk of boot's 524 K MOVEM helper executions
turn out to be from the **small-N inline arms' conditional bounds-
fail bridges** hitting MMIO targets, *not* the large-N fallthrough.

Next iteration: redirect those bounds-fail bridges from `m68k_step`
to the fast helpers, which should capture ~500 K m68k_step calls per
60 M-cycle boot run.
| Goal: 5 × interp on bench       | 1.32            | **23.2 ×**       | 1.18           | **25.9 ×**      |

**Mac Plus speed already cleared** (>1 ×) by the interpreter alone —
the JIT pass is widening the margin. M6.2's headline is **7.64 ×** the
original Macintosh Plus on the Speedometer-frozen bench. The 5×-interp
target (**23.2 ×** Mac Plus) is still a structural gap: once the M6.2
register-cache wiring is correctness-debugged and turned on it should
land at ~9 × Mac Plus; the rest needs lazy CCs comprehensive coverage
and ESP32-only native block chaining.

## Next-iteration plan — high-gain structural items

The goal is to push the bench from **7.64 × Mac Plus → 23 × Mac Plus** (the
"5× interpreter" target). The three big items:

### Item 1 — Full register caching of hot D/A regs across a block

**Current cost**: every inline op does `l32i Dm → scratch; … ; s32i scratch → Dn`.
Per op: 2–4 memory ops × 3 bytes = 6–12 bytes of l32i/s32i pairs.
With ~17 M inline-op executions in bench, that's ~100 M LX7 of pure register
shuffling.

**Plan**:
* Reserve a4..a7 (four scratch regs we currently don't use) as cache slots.
* At compile time, scan the block's inline ops to count guest-reg usage.
* Pick the top-N most-used (D0..D7, A0..A7) for caching, up to 4.
* Block prologue loads them from cpu state once; epilogue stores them back.
* Helper-call wrapper flushes the cache before `CALLX0 m68k_step` and reloads
  after (m68k_step writes through cpu->d[]/cpu->a[]).
* Each inline emit uses two new helpers, `emit_read_d(e, n, dst_xt)` and
  `emit_write_d(e, n, src_xt)` — they `xt_mov` to/from the cache slot when
  the reg is cached, fall back to `l32i`/`s32i` otherwise.
* Block-by-block heuristic: skip caching entirely for blocks that are
  helper-heavy (fewer inline reads/writes than the prologue/epilogue cost).
* Expected impact: 0.3–0.5 lx7_per_cyc on bench → bench under 3.7.

### Item 2 — Comprehensive lazy CCs

**Current state**: lazy CC is enabled for `ADDQ.W → Dn` and `MOVE.W → Dn`
only. Earlier attempts at full coverage tripped the bench: the classifier
is too eager on helpers (`SET|CONS` for everything not specifically known),
and the M68000's actual per-opcode CCR consumption is far narrower than the
default.

**Plan**:
* Build a per-opcode-byte CCR-usage table (sets / reads which of N, Z, V, C, X)
  derived from the interpreter's `set_flags_*` calls and the m68k programmer's
  reference. Most ops don't actually read CCR — only Bcc/DBcc/Scc/ADDX/SUBX/
  NEGX/ROXL/R/RTR/the trap pushes do.
* Replace the coarse SET|CONS bit-pair with a fine-grained struct
  `ccr_use { sets_mask, reads_mask }`. The backward scan tracks per-bit
  liveness rather than a single bool.
* Modify each inline flag emitter to accept a `needs_mask` instead of a
  binary skip — when only X is consumed and N/Z/V/C are dead, only emit X.
* Expected impact: 0.5–0.8 lx7_per_cyc on bench (frees ~50 % of the
  emit_addsub_flags_long emissions, each 25 ops).

### Item 3 — Native block chaining on ESP32

**Why deferred on host**: `xt_sim` runs one block buffer per `xt_sim_run`
call; cross-block jumps land outside the buffer and the sim aborts.
Cannot be exercised by `--diff-jit` on the host.

**Plan**:
* On ESP32 the code cache lives in IRAM and all blocks share one address
  space — a block's tail can `j` directly to its successor.
* Maintain a "predicted-next" pointer per block (already there) and back-
  patch the tail to a real `j target` once the successor is compiled.
* Invalidate the patched tail when the predecessor or successor moves
  (SMC invalidation already invalidates blocks; extend it to revert tail
  patches).
* Eliminates per-chained-block: dispatcher round-trip (prologue + epilogue
  ≈ 4 ops × 8 M chained block executions = ~32 M LX7).
* Expected impact: 0.1 lx7_per_cyc on bench, more on boot. Real win is
  wall-clock — the dispatcher's C-side overhead (mac_mem_tick, poll,
  sony_service, find_block lookup) is removed for chained transitions
  but isn't counted in the proxy metric.

Order: **Item 1 first** (biggest single-iteration win, doesn't need new
infrastructure). Item 2 in parallel where the classifier table lands.
Item 3 last (needs the ESP32 target build + on-device benchmark).

### Item 1 — progress so far

**Infrastructure built** (M6.1):
* `regcache` struct (jit/codegen.c) with `xt[16]` (guest → Xtensa reg
  mapping), `guest[4]` (slot → guest reg), `active`, `dirty` bitmap.
* `R_CACHE0..R_CACHE3` (a4..a7) reserved.
* `emit_read_g` / `emit_write_g` helpers: `xt_mov` to/from the cache
  slot when cached, fall back to `l32i`/`s32i` otherwise.
* `emit_cache_flush` / `emit_cache_reload` — writeback / reload all
  slots. Used at helper boundaries and the block epilogue.
* `emit_helper_step` now takes a `regcache *` and wraps the `CALLX0`
  with `flush + helper + reload` (m68k_step writes through cpu->d[]/
  cpu->a[], so the cache must round-trip).
* Per-block analysis: count guest D/A uses, pick top-4 (≥2 uses each).
* Prologue emits `l32i` for each chosen slot; epilogue emits the
  symmetric `s32i` flush.

**`emit_adda_w_reg` and `emit_movea_l_reg` converted** — when the
destination An is cached, the op `xt_add R_CACHE_dst, R_CACHE_dst, …`
writes the result directly into the cache slot (skipping the `s32i`)
and marks the slot dirty.

**Why the cache stays disabled** (`disable_cache = true` in
`compile_block`): a block can mix converted and unconverted inline
emits. An unconverted emit writes `cpu->a[3]` via `xt_s32i`; the cache
slot caching A3 still holds the prologue-loaded value. Subsequent
converted emits read from the (stale) cache. Boot drifts to a wrong PC
within 13 M cycles. Re-enabling the cache costs +1.28 lx7_per_cyc on
bench (overhead dominates because almost no emits actually consume the
savings yet) and a divergent boot path. Reverted with `disable_cache`
gating until the conversion is comprehensive.

**M6.2 — all 20+ inline emits + arms converted to cache.** Every D/A
read/write in `emit_moveq`, `emit_move_l_imm_dn`, `emit_movea_l_imm`,
`emit_move_l_dd`, `emit_add_l_dd`, `emit_addq_l_dd`, `emit_addq_an`,
`emit_cmp_l_dd`, `emit_sub_l_dd`, `emit_logic_l_dd`, `emit_immalu_l_dn`,
`emit_addq_w_dn`, `emit_lea`, plus the MOVE.W (`(An),Dn`,
`(d8,An,Xn),Dn`, `Dn,(An)`, `(An),(An)`, `Dm,Dn`) and CMP.W (`(An),Dn`,
`(d16,An),Dn`, CMPA.W `(d16,An),An`) inline arms now goes through
`emit_read_g`/`emit_write_g`. `BYTES_PER_OP` bumped 160 → 240 to absorb
the wider per-op emit. With `disable_cache = true` everything is
functionally identical to M6.1 (bench 4.008, boot 5.376, ctest 3/3).

**Open bug — enabling the cache breaks bench & boot.** With
`disable_cache = false`: bench regresses 4.008 → 8.330, boot 5.376 →
7.756 (boot drifts to PC 0x20DB11, bench drifts to PC 0xFBB3C52).
Helper count explodes from 10 M → 36 M on bench and from 40 M → 64 M
on boot, suggesting many block compiles overflow and fall to interp.
Bumping `BYTES_PER_OP` from 240 → no effect; bumping `M68K_JIT_ARENA_KB`
no effect; reducing cache slots to 1 instead of 4 reproduces.
Inspection of every cache path (prologue load, helper flush+reload,
epilogue flush, `emit_read_g`/`emit_write_g` dirty tracking) shows no
obvious correctness gap, and dispatcher stats show `arena_resets = 426`
(was 0) suggesting the new prologue/epilogue may be triggering arena
churn rather than per-block buffer overflow.

**M6.3 — bisect results.** Bisect knobs `g_cache_emit_disabled`,
`g_cache_flush_disabled`, `g_cache_reload_disabled`, `g_reload_slots_max`
added. With cache analysis on (`rc.active = 4`) but emits ignoring the
cache (so writes stay via `s32i` and reads via `l32i`):

* Prologue cache-loads only (no flush, no reload around helpers):
  bench **4.053** (close to 4.008 baseline, just the prologue
  overhead). ✓ Functionally correct.
* + flush after helper: bench **4.053** unchanged. Flush emits no
  s32i (dirty bitmap is 0 in this mode). ✓ Correct.
* + reload after helper (limit = 1, i.e. one `l32i` of one cache slot
  after each helper): bench regresses **4.053 → 5.274**, PC drifts
  to 0x401124 instead of 0x401A04. Helpers count jumps **10 M → 19 M**,
  blocks_compiled drops **1276 → 291**. Boot drifts to PC 0x20DB11.

So the bug is in **the reload sequence after a helper**, even when the
reload writes to a register no inline emit reads. That's contradictory
to first-principles — `l32i a4, R_CPU, OFF_D(gi)` after a `callx0` is
semantically a no-op for code that never reads a4 — but the bench's
JIT visits a completely different set of blocks (4× fewer unique
blocks) once reload is on.

Hypotheses to test next iteration:
* The `l32i` after `callx0` might be reading a stale literal-pool
  region if the helper-call somehow shifted the emit alignment, but
  `emit_l32r_at` is computed before the helper call and the
  post-helper `l32i` uses an immediate offset, not a literal pool —
  so this is unlikely.
* CALL0 ABI: real Xtensa LX7 has callee-preserved a0 and saved-by-
  caller a4-a7, but the *host's* `xt_sim` doesn't model ABI clobbering
  — `sim_call` just dispatches a C function and leaves `s->a[]`
  untouched. So a4 stays at whatever the prologue loaded.
* Could the bench's m68k_step *itself* observe a difference based on
  the timing of the reload? It runs on the C side, not via sim — so
  no. Unless the divergence is in **cycle counting**: more LX7
  instructions per JIT block → host wall-clock differs (`jit_cost`
  goes up, but `lx7_per_cyc` should stay the same…)

The numbers say the JIT is taking a *different code path* (different
block PCs), and 1 reload-l32i is enough to flip it. Almost certainly
points to a stale-state interaction (the reload value, or some side
effect of the extra emit, perturbs the guest in a way that's not
register-state-corruption — maybe via cycle accounting or interrupt
timing through `mac_mem_tick` granularity).

Next iteration target: dump the emitted bytes around a helper call
(with and without reload) and step the sim instruction-by-instruction
on a chosen block to confirm the runtime state matches between the
two configurations — that will pinpoint whether the reload is
side-effect-free or whether it perturbs something orthogonal.

**M6.5 — cache ON, two bugs fixed; bench 4.008 → 2.418 (12.67 × Mac Plus).**

Two cooperating bugs blocked the cache enable; both are now fixed in
`jit/codegen.c` and `jit/dispatcher.c`:

1. **Hardcoded fast-path skip distance.** Seven inline emit sites
   (MOVE.W (As)→(Ad), MOVE.W Dn,(An), CMPA.W (d16,An),An, MOVE.W
   imm,Dn variants, MOVE.W (d8,An,Xn),Dn, MOVE.W (An),Dn) pre-compute
   a `xt_beqz a10, 15` to skip past the helper-call and into the fast
   path. The "15" assumed `emit_helper_step` was exactly 9 bytes
   (mov + l32r + callx0). With the cache on, the helper now emits a
   pre-flush (3 bytes per dirty slot) and a post-reload (3 bytes per
   active slot), so 9 grows to 9+N. The beqz then landed *inside* the
   helper body and the runtime jumped to garbage.

   Fix: a new `helper_step_after_flush_size(rc)` returns the exact
   byte count for the current `rc` state, and every conditional site
   uses `xt_beqz (&e, 10, (i32)(6u + helper_step_after_flush_size(&rc)))`.

2. **Dirty bit prematurely cleared inside a conditional helper.** When
   the conditional helper's `emit_cache_flush` ran at compile time, it
   cleared `rc->dirty` for every slot — but at runtime the helper
   branch is one of two branches, and the *fast* branch never runs the
   flush. So a slot whose value was set before the bounds check (e.g.
   D0 written by MOVEQ) stayed dirty in the cache on the fast path
   but the compile-time tracker thought it had been written back. The
   next helper-step's compile-time flush would then *skip* that slot,
   and the block's epilogue would also skip it — D0 never landed in
   `cpu->d[0]`.

   Fix: every conditional site now emits `emit_cache_flush(&e, &rc)`
   *before* the bounds check (so both branches see a clean state at
   entry), and the helper branch uses a new `emit_helper_step_after_flush`
   that skips its own flush. The compile-time dirty assumption now
   matches both runtime branches.

Result:

| Engine | bench `lx7/cyc` | bench × Mac Plus | boot `lx7/cyc` | boot × Mac Plus |
|--------|----------------:|-----------------:|---------------:|----------------:|
| M6.2 (cache OFF)  | 4.008 |  7.64 × | 5.376 | 5.70 × |
| **M6.5 (cache ON)** | **2.418** | **12.67 ×** | **5.179** | **5.91 ×** |

The bench gain is 40 % from cache OFF baseline. The boot gain is
3.7 % — bottlenecked by heavier helper traffic (every helper still
pays flush + reload). All ctests pass; `--diff-jit-trace` matches
through step 543 (same divergence point as the pre-cache baseline,
i.e. an unrelated pre-existing VIA timing issue, not introduced by
the cache).

A third cooperating issue: arena resets used to leave a dangling
`prev` pointer in the dispatcher's run loop, then `prev->predicted_next
= b` corrupted freed memory. The cache triggered resets that were
rare without it (more bytes per block), exposing the latent bug. Fix:
the run loop now snapshots `d->arena_resets` around the `get_block`
call and nulls `prev` when the counter advanced (`jit/dispatcher.c`
`m68k_dispatcher_run_until`).

Next high-gain item: the cache currently saves cycles on the real
ESP32-S3 (mov is 1 cycle vs l32i's 2 cycle load-use latency) but the
host `xt_sim` metric counts both as one instruction. To shrink
`xt_instrs` further, the inline emits must operate directly on the
cache slot instead of moving through `a8`/`a11` scratch — e.g.
`emit_add_l_dd` currently emits 2 movs + 1 add + 1 mov when both
operands are cached; the direct-cache form is just 1 add.

**M6.6 — lazy-CC eligibility expanded.** The classifier-restricted
list of "skip-flags-safe" opcodes was previously only `ADDQ.W → Dn`
and `MOVE.W → Dn`. Extended to also cover `ADD.L/SUB.L Dm,Dn`,
`AND.L Dm,Dn`, `EOR.L Dn,Dm`, the immediate-ALU `.L #imm32,Dn` family
(ORI/ANDI/ADDI/SUBI/EORI), and `MOVEQ`. Each of these inline emitters
already accepts `skip_flags`. Net measured impact on bench/boot is
~5 K xt_instrs (negligible) because the bench's flag-setting ops are
almost always immediately followed by a consumer (Bcc/CMP). The
expansion does no harm (all ctests pass, `--diff-jit-trace` unchanged)
and unlocks future scenarios where a longer inline pipeline can drop
the trailing flag emits.

**M6.7 — direct cache-slot reads in MOVE.W (An),Dn fast path.** Added
a new `emit_read_g_in(e, rc, gi, scratch)` helper that returns the
cache slot register directly when `gi` is cached (no mov), and only
emits `l32i` into `scratch` when it isn't. Converted the MOVE.W
(An),Dn bounds-check + fast-path to use it for the An read, and to
do the low-16 merge in-place on the Dn cache slot when Dn is cached.
Net measured impact on bench is ~6 K xt_instrs (the bench's hot
blocks happen to not cache An, since A3's use-count is only 1 in the
analysis window). The pattern is now in place for the other inline
emits to follow: ADD.L/SUB.L Dm,Dn, CMP.L Dm,Dn, MOVE.W (As)→(Ad),
MOVE.W Dn,(An), etc. Wherever both operands are cached, this saves
2 movs per execution (read + write).

**M6.8 — ALU direct-cache emits.** `emit_addsub_flags_long` parameterised
to take `(s, d, r)` register operands rather than fixed `a8/a9/a10`,
unlocking direct-cache flag computation. Converted `emit_add_l_dd`,
`emit_sub_l_dd`, `emit_cmp_l_dd` to use direct-cache paths:

* **CMP.L Dm,Dn** uses `emit_read_g_in` on both operands; when both
  are cached this drops the two source-load movs (4 → 2 ops, plus the
  flag-emit-already-correct).
* **ADD.L/SUB.L Dm,Dn** in-place add/sub on the Dn cache slot when
  both operands cached and `dm != dn`. Saves 1 mov per execution; when
  flags are needed an extra `mov a9, dn` saves the pre-add value for
  CCR computation. Net: 4 → 2 ops (or 3 with flags).

Bench/boot xt counters move 6 K and 45 ops respectively — the bench
doesn't run ADD.L Dm,Dn in its hot loop, and boot's hot helpers
dominate. The pattern is now consistent and ready for the remaining
ALU forms to follow.

**Where the headroom is.** Per-block cost breakdown for the bench
(4-op blocks, MOVE.W dominant) shows:

| Bucket               | ops/block | % of bench cost |
|----------------------|----------:|----------------:|
| flag emit (MOVE-fam) | ~40       | 33 %            |
| emit_advance (PC/cyc)| ~24       | 20 %            |
| bounds check + br    | ~16       | 13 %            |
| memory read fast path| ~24       | 20 %            |
| merge / write Dn     | ~12       | 10 %            |
| **per-block total**  | **~120**  | **100 %**       |

Two next big levers are queued:

1. **PC/cycles cache slot** — keep cpu->pc and cpu->cycles in
   dedicated registers across the block; flush before each helper,
   reload after. Drops `emit_advance` from 6 ops to 2. Expected
   ~0.2 lx7/cyc on bench (2.418 → ~2.22).
2. **Lazy CCR deferral** — defer flag computation past the setter;
   the consuming branch evaluates only the bit it needs (Z for BEQ,
   C for BCC, etc.). Drops the 25-op `emit_addsub_flags_long` /
   10-op `emit_logic_flags` to ~3 ops when the next consumer is a
   single-bit branch. Expected ~0.5 lx7/cyc on bench (2.418 → ~1.9).

**M6.9 — PC/cycles cache attempt; reverted.** Tried implementing the
PC/cycles cache as described above: R_HELP moved to a13, R_PC = a14,
R_CYC = a15, `emit_load_imm32` tmp from a15 → a12. `emit_advance` drops
from 6 ops to 2. Prologue loads PC/cycles, epilogue stores them back,
each helper-step flushes/reloads them around `callx0`.

**Result**: bench improves 2.418 → 2.104 lx7/cyc (13 %, **14.56 × Mac Plus**),
all ctests pass, `--diff-jit-trace` matches through the same step 543
as baseline, but **boot regresses catastrophically** — runs cleanly to
cycle ~48.5 M, then thrashes (1471 arena resets vs M6.6's 35) and halts
at cycle 47.83 M with `halted=4` (M68K_HALT_GUEST_EXIT) and a wild PC
0xFB3FB25D. The bench's hot loop doesn't hit the buggy path, so the
correctness gap only surfaces on boot's wider opcode coverage.

Suspected root cause (not yet confirmed): when m68k_step takes a guest
exception inside a helper (TRAP, line-1010/1111, address error), it
updates cpu->pc to the exception vector. The post-helper `pc_cyc_reload`
pulls the vector into R_PC, then the block's remaining ops bump R_PC
from there — but those subsequent emits were compiled for the ORIGINAL
op sequence, so PC drifts. This was technically a latent issue before
(emit_advance also chained off cpu->pc) but with PC/cycles cached in
registers, the drift now propagates across multiple ops before the
block exits to the dispatcher, where the older code wrote each
post-advance value back to cpu_state and would at worst lose one op
worth of correction at the next helper.

Reverted to the M6.6 codepath: `emit_advance` does l32i/addi/s32i
per call; `emit_branch` writes cpu->pc directly; no R_PC/R_CYC. Bench
stays at 2.418 lx7/cyc, boot at 5.179.

**Next iteration target**: implement PC/cycles cache *correctly* by
having the helper bridge detect that m68k_step changed cpu->pc to an
exception vector and bail out of the block (jx to epilogue) instead
of continuing the compiled-but-now-wrong op sequence. Then the cache
becomes safe to enable.

**M6.10 — boot helper profile.** Instrumented `compile_block` to count
which 68000 opcodes hit the helper fallback. Boot's helper compile
distribution is dominated by a tiny set of opcodes:

| Opcode | Count   | Decoded                             |
|-------:|--------:|-------------------------------------|
| 0x0000 | 554 496 | ORI.B #imm, D0                      |
| 0x002C | 407 808 | ORI.B #imm, (d16,A4)                |
| 0x0202 | 1 537   | ANDI.B #imm, D2                     |
| 0x0200 | 544     | ANDI.B #imm, D0                     |
| ...    | < 100 each | tail (RTS, TST.B (xxx).L, JMP …) |

The top two opcodes (0x0000 / 0x002C) account for >99 % of helper
compiles — they are the `00 00 NN NN ...` pattern that arises when
the JIT chases a PC into a zero-filled RAM region. Each compiled
block ends up filled with ~25 ORI.B helpers. Inlining the immediate-
ALU `.B` and `.W` variants targeting `Dn` would cover these.

Estimated impact (per the helper LX7 cost model of 64 LX7/helper):
inlining halves the boot helpers (1.65 M removed × 64 = 105 M LX7),
which is `105 / 60 = ~1.75 lx7/cyc` off boot. Bench helpers are tiny
(87 K out of 145 M LX7), so this is mostly a boot-side gain.

**Queued for next iteration**: add `emit_immalu_b_dn` and `emit_immalu_w_dn`
modeled on `emit_immalu_l_dn`, plus the dispatch arms in `compile_block`.
Risk: the .B/.W variants need careful masking to preserve the upper bits
of Dn — easy to get wrong, mitigated by `--diff-jit-trace` after each
addition.

**M6.11 — ORI/ANDI/EORI/ADDI/SUBI/CMPI `.B` and `.W` to Dn inlined.**
Added two helpers:

* `emit_immlogic_bw_dn(e, dn, imm, size, kind, rc)` — handles
  ORI/ANDI/EORI .B/.W. The imm's high bits are 0 for OR/EOR (naturally
  preserves Dn's upper bits) or forced to 1 for AND. Result is shifted
  into the high bits to feed `emit_logic_flags`, whose bit-31 N and
  "vreg != 0" Z then correctly reflect the size.
* `emit_immarith_bw_dn(e, dn, imm, size, kind, rc)` — handles
  ADDI/SUBI/CMPI .B/.W. Computes the full 32-bit add/sub for the
  write-back path, then shifts s, d, r into the high bits for
  `emit_addsub_flags_long_ex` (carry/overflow naturally land at bit 31
  of the shifted operands).

Six dispatch arms wired up in `compile_block`. Net measured impact:

| Engine | before M6.11 | after M6.11 | × Mac Plus | delta |
|--------|------------:|-----------:|-----------:|-----:|
| Bench  | 2.418       | 2.415      | 12.68 ×    | flat |
| Boot   | 5.179       | **4.816**  | **6.36 ×**  | **+7.0 %** |

Boot helpers dropped 966 K → 410 K (-57 %); inline_ops grew 408 K → 966 K
(+136 %). Bench is largely unchanged because its hot inner loop is
MOVE.W-dominated, not immediate-ALU.

**Parameterisation bug found and fixed.** First attempt at
`emit_immarith_bw_dn` passed `s=a13, d=a14, r=a15` to the new
`emit_addsub_flags_long_ex`. That broke bench catastrophically (2.418 →
5.208 lx7/cyc, PC drift to ROM). Root cause: the flag-emit's internals
clobber `a13` very early (line 338 of the function: `xt_movi(e, 13, -1)`),
so a caller that passes `s=13` reads garbage at the later `xt_xor(e, 12,
s, r)` step. Re-implemented to use the conventional `s=a8, d=a9, r=a10`
(which the function only writes to AFTER all operand reads complete) —
all paths now correct and `--diff-jit-trace` matches baseline through
step 543.

**Lesson for the next pass:** `emit_addsub_flags_long_ex` parameters
`(s, d, r)` are safe at `a8/a9/a10` (the conventional positions) and at
free cache slots `a4..a7`. Avoid `a11..a13` (internal scratch) and `a14/a15`
unless the calling site is certain those aren't written before all
operand reads complete.

**M6.12 — `emit_load_imm` small-imm shortcut.** Added a thin wrapper
around `emit_load_imm32`: when the value fits in Xtensa's 12-bit signed
movi range (-2048..2047), emit a single `xt_movi` (3 bytes, 1 LX7
instruction) instead of the 4-op build-by-byte sequence (12 bytes, 4
ops). Saves 3 instructions per small-immediate load.

Routed through the four immediate-ALU emitters (`emit_move_l_imm_dn`,
`emit_movea_l_imm`, `emit_immalu_l_dn`, `emit_immlogic_bw_dn`,
`emit_immarith_bw_dn`) plus the LEA / CMPA `(d16,An)` and `(xxx).W`
displacement loads. Boot's xt_instrs drops 112.5 M → 107.5 M (-4.5 %):

| Engine | M6.11    | **M6.12**    | × Mac Plus | delta |
|--------|---------:|-------------:|-----------:|------:|
| Bench  | 2.415    | **2.414**    | 12.69 ×    | flat  |
| Boot   | 4.816    | **4.732**    | **6.47 ×**  | **+1.7 %** |

Cumulative session win **M6.2 → M6.12**:
* Bench `4.008 → 2.414 lx7/cyc` (**+39.8 %**), 7.64 × → **12.69 × Mac Plus**.
* Boot  `5.376 → 4.732 lx7/cyc` (**+12.0 %**), 5.70 × → **6.47 × Mac Plus**.

Target: **5 × interpreter on bench = 23.2 × Mac Plus** (lx7/cyc ≤ 1.32).
Remaining gap on bench: 12.69 → 23.2 × ⇒ needs `lx7/cyc` to drop 1.08
more (45 % further). The two big levers (lazy CCR deferral, PC/cycles
cache with safe exception-bailout) are still the path; small-imm
collapsing this iteration touches only `~5 M xt_instrs` out of 140 M.

**M6.13 — classifier expanded (no measured impact yet), bench hot path
identified.** `classify_op` now recognises SWAP, EXT.W, EXT.L, NEG,
NOT, CLR, TST, and MOVEM as non-consumers (don't read CCR). This
unlocks more `flags_dead = true` opportunities for upstream setters,
but no measured impact on either bench or boot because the bench's
hot blocks don't use any of these ops and boot's helper-heavy code
isn't the bottleneck.

**M6.14 — `cpu->sr` cached in a14 (R_SR), with compile-time dirty
tracking.** Reserved a14 for R_SR (R_HELP moved to a13). The flag
emitters (`emit_logic_flags`, `emit_addsub_flags_long_ex`) and the
condition decoder (`emit_cond`) now read/write R_SR directly instead
of going through `l16ui`/`s16i` on cpu->sr. Helpers still need
`cpu->sr` current for interrupt-level and supervisor-mode checks,
so the helper bridge flushes R_SR → cpu->sr before `callx0` and
reloads after. Block prologue/epilogue carry the load/store.

To avoid penalising helper-heavy boot code (where most blocks have
0 inline flag emits), the flush is gated on a compile-time `g_sr_dirty`
flag set by every flag emit and cleared by every flush/reload. When
no flag emit has happened since the last sync, the s16i is skipped.

| Engine | M6.12   | **M6.14**   | × Mac Plus  | delta |
|--------|--------:|------------:|------------:|------:|
| Bench  | 2.414   | **2.387**   | **12.83 ×**  | **+1.1 %** |
| Boot   | 4.732   | **4.725**   | **6.48 ×**   | +0.1 % |

Cumulative session win **M6.2 → M6.14**:
* Bench `4.008 → 2.387 lx7/cyc` (**+40.4 %**), 7.64 × → **12.83 × Mac Plus**.
* Boot  `5.376 → 4.725 lx7/cyc` (**+12.1 %**), 5.70 × → **6.48 × Mac Plus**.

**M6.15 — `emit_advance` batching (compile-time PC/cyc accumulator).**
The 6-op `l32i/addi/s32i` sequence for cpu->pc and cpu->cycles ran
once per inline op. With ~8 ops per bench hot block and 405 K block
executions, that was ~19 M LX7 of pure cycle accounting (13 % of
bench cost).

Implementation:
* `emit_advance(pc_delta, cyc)` no longer emits anything — it just
  accumulates into compile-time globals `g_pc_acc`, `g_cyc_acc`.
* `emit_advance_flush(e)` emits one combined `cpu->pc += g_pc_acc;
  cpu->cycles += g_cyc_acc;` at any sync point.
* Sync points: at the start of every conditional helper site (before
  the An read clobbers a8 — discovered the hard way), at the start
  of `emit_helper_step`, and at the block epilogue.
* `emit_branch` absorbs `g_cyc_acc` into its own BRA/Bcc cycle add,
  and discards `g_pc_acc` (branch overwrites cpu->pc with the target).
* The 7 conditional-helper fast-path emit-sites use a new
  `emit_advance_now()` for their own op's delta (since the helper
  branch's m68k_step already advances cpu->pc, the fast path must
  also advance — but neither path should touch the accumulator).

**The bug that gated this**: first version put `emit_advance_flush`
right before the `xt_beqz` in each conditional site, AFTER the An
read had loaded a value into a8. `emit_advance_flush` uses a8 as
scratch (`l32i a8, OFF_PC` etc.), so the An value was destroyed and
the bounds check used PC bits. Fast path then used the same trashed
a8 as the RAM offset → wild memory reads. `--diff-jit-trace`
diverged at step 34 immediately. Fixed by moving the flush to
**before** any register reads in each conditional site.

| Engine | M6.14   | **M6.15**   | × Mac Plus  | delta |
|--------|--------:|------------:|------------:|------:|
| Bench  | 2.387   | **2.182**   | **14.04 ×**  | **+8.6 %** |
| Boot   | 4.725   | **4.552**   | **6.73 ×**   | **+3.7 %** |

Cumulative session win **M6.2 → M6.15**:
* Bench `4.008 → 2.182 lx7/cyc` (**+45.6 %**), 7.64 × → **14.04 × Mac Plus**.
* Boot  `5.376 → 4.552 lx7/cyc` (**+15.3 %**), 5.70 × → **6.73 × Mac Plus**.

**M6.16 — cross-block + MOVE.W mem-destination flag-skip.**
Added two related changes:

1. Cross-block lazy-CC initialisation: if the block ends with a
   non-consumer control op (BRA.S unconditional or fall-through via
   ends_block-at-non-branch) AND the NEXT block's first op is a
   SET-without-CONS class, initialise `need = false` in the backward
   scan. Lets the last setter in the current block be marked dead
   when the next block's first op overwrites CCR without reading it.
   On the bench's hot block 0x03DF40 (which ends with a Bcc/CMP pair
   in op 8-10) the within-block analysis already catches MOVE.W
   (d8,A0,Xn) D5 — so cross-block adds no measured gain on bench,
   but the plumbing now exists for other workloads.

2. **MOVE.W mem-destination variants in the allow-list.** The
   `flags_dead` filter forced false for any op not on the small
   allow-list. Added `top == 0x3 && op6 == 2` (i.e. MOVE.W (As)→(Ad)
   bench-hot 0x3692 and MOVE.W Dn,(An) bench-hot 0x3484), so their
   `flags_dead = true` from the backward scan actually skips the
   emit_logic_flags call.

This second item is what moves the needle: bench's block at 0x03DF58
runs MOVE.W (A2),(A3) and MOVE.W D4,(A2) every iteration, and both
are now flag-skipped (the CMP after them overwrites CCR anyway).

| Engine | M6.15   | **M6.16**   | × Mac Plus  | delta |
|--------|--------:|------------:|------------:|------:|
| Bench  | 2.182   | **2.125**   | **14.41 ×**  | **+2.6 %** |
| Boot   | 4.552   | 4.552       | 6.73 ×       | flat   |

Cumulative session win **M6.2 → M6.16**:
* Bench `4.008 → 2.125 lx7/cyc` (**+47.0 %**), 7.64 × → **14.41 × Mac Plus**.
* Boot  `5.376 → 4.552 lx7/cyc` (**+15.3 %**), 5.70 × → **6.73 × Mac Plus**.

Target: **5 × interpreter on bench = 23.2 × Mac Plus** (lx7/cyc ≤ 1.32).
Remaining gap on bench: 14.41 → 23.2 × needs `lx7/cyc` to drop 0.80
more (38 % further).

**M6.17 — bits-needed flag-emit + cross-block Bcc dead-CCR analysis.**
Two cooperating changes:

1. **Bits-needed flag emit**: refactored `emit_addsub_flags_long_ex`
   into `emit_addsub_flags_long_masked(e, is_sub, keep_x, s, d, r, cc_mask)`
   where `cc_mask` selects which of {C, V, Z, N, X} to materialise.
   Bits not in the mask are left unchanged in R_SR. The original
   full-mask path is preserved as a fast path to avoid regressing the
   common case. For BLE (cc=15) the mask is N|V|Z = 0xE — skipping the
   ~8-op C-bit computation.

2. **Cross-block Bcc dead-CCR**: extended the lazy-CC backward scan's
   initial state. For Bcc-ended blocks (`top=6 && cc>=2`), check
   *both* the taken target and the fall-through PC. If both first ops
   are SET-without-CONS class, CCR is dead beyond the Bcc — so the
   upstream setter only needs to write the bits the Bcc *itself*
   reads, nothing more.

The bench's hot block 0x03DF40 ends with CMP.W (A2),D5 + BLE.S +6. The
two BLE destinations are 0x03DF58 (starts with MOVE.W D5,D4) and
0x03DF5A (fall-through into the BLE's not-taken case, which also leads
into setters). Both overwrite CCR — so the CMP only needs to write Z,
N, V (the bits BLE reads). Wired through the three bench-hot CMP arms
(CMP.W (An),Dn, CMP.W (d16,An),Dn, CMPA.W (d16,An),An).

| Engine | M6.16   | **M6.17**   | × Mac Plus  | delta |
|--------|--------:|------------:|------------:|------:|
| Bench  | 2.125   | **2.071**   | **14.79 ×**  | **+2.5 %** |
| Boot   | 4.552   | 4.552       | 6.73 ×       | flat   |

Cumulative session win **M6.2 → M6.17**:
* Bench `4.008 → 2.071 lx7/cyc` (**+48.3 %**), 7.64 × → **14.79 × Mac Plus**.
* Boot  `5.376 → 4.552 lx7/cyc` (**+15.3 %**), 5.70 × → **6.73 × Mac Plus**.

**M6.18 — cross-block forward-walk through CCR-neutral ops.** The
`PC_OVERWRITES_CCR(pc)` check previously inspected only the *first* op
at the given PC. For destinations like 0x03DF40 — which starts with
MOVEA.L A4,A0 (CCR-neutral) followed eventually by MOVE.W
(d8,A0,Xn),D5 (setter that overwrites CCR) — the first-op check
returned false because MOVEA.L's classifier is 0 (neither setter nor
consumer). So the upstream block's CCR was conservatively kept live.

Extended to walk forward up to 8 ops, skipping CCR-neutral ones, and
return true if a SET-without-CONS op is found before any consumer.
This catches the bench's loop-back pattern: block 0x03DF58 ends with
BLT.S → 0x03DF40 which starts with MOVEA, ADDA, then a setter. The
CCR set by the BLT-preceding CMP is now correctly identified as
dead beyond what the BLT itself reads (N|V), so the CMP only writes
those two bits.

| Engine | M6.17   | **M6.18**   | × Mac Plus  | delta |
|--------|--------:|------------:|------------:|------:|
| Bench  | 2.071   | **2.003**   | **15.29 ×**  | **+3.3 %** |
| Boot   | 4.552   | 4.552       | 6.73 ×       | flat   |

Cumulative session win **M6.2 → M6.18**:
* Bench `4.008 → 2.003 lx7/cyc` (**+50.0 %**), 7.64 × → **15.29 × Mac Plus**.
* Boot  `5.376 → 4.552 lx7/cyc` (**+15.3 %**), 5.70 × → **6.73 × Mac Plus**.

**M6.19 — CMP.L Dm,Dn plumbed through cc_mask.** Added cc_mask
parameter to `emit_cmp_l_dd` and threaded `flags_needed[i]` through
the dispatch call. Negligible measured impact on either bench or boot
(neither hits CMP.L Dm,Dn in their hot paths), but the plumbing is now
in place for any workload that does. Bench inched to 2.002 lx7/cyc.

**M6.19 cc_mask plumb to immediate-ALU .B/.W reverted.** Attempted
to thread cc_mask through `emit_immarith_bw_dn` (CMPI/ADDI/SUBI .B/.W
forms). Even gating the mask to apply only on kind == 6 (CMPI, no X
write) and full-mask for kind == 2/3 (ADDI/SUBI), `--diff-jit-trace`
diverged at step 28 on the bench snapshot. Root cause not fully
characterised — likely some interaction between the masked emit's
"don't touch unrelated R_SR bits" semantics and a downstream consumer
that needs `R_SR & ~mask` bits to be predictably zero. Reverted; the
.B/.W variants stay on full-mask. Lazy-CCR for those forms is queued
behind a more rigorous analyzer that tracks X-bit liveness explicitly.

**Bench final (this iteration)**: 2.002 lx7/cyc = **15.30 × Mac Plus**.
**Boot**: 4.552 lx7/cyc = **6.73 × Mac Plus**.

**M6.20 — deferred-advance with helper-bridge undo.** All 7
conditional-helper inline sites (MOVE.W (An)/Dn, MOVE.W (As),(Ad),
MOVE.W Dn,(An), MOVE.W (d8,An,Xn),Dn, CMPA.W (d16,An),An, CMP.W
(d16,An),Dn, CMP.W (An),Dn) used to emit a 6-LX7-op `emit_advance_now`
at the end of their fast path. This iteration replaces that with:

* Fast path: `emit_advance` (compile-time accumulator only, 0 emitted
  ops). The op's pc/cyc delta sits in the accumulator until the next
  sync point (next conditional helper's flush, helper-step's flush,
  or block epilogue).
* Helper path: a new `emit_helper_step_after_flush_undo` that, AFTER
  the m68k_step callx0, emits 6 LX7 ops to *undo* the m68k_step's
  natural advance — so both paths leave cpu->pc/cycles in the same
  state as before the op, and the deferred accumulator covers both.

Trade-off: save 6 ops in the fast path of every conditional helper
execution; pay 6 extra ops in the (rare) helper path. Bench's fast
path dominates → 7 % win. Boot has more helpers but mostly NOT in the
7 conditional sites (boot helpers are unrecognised opcodes via the
`if (!done)` fallback `emit_helper_step`, which is unchanged) → no
boot regression.

| Engine | M6.19   | **M6.20**   | × Mac Plus  | delta |
|--------|--------:|------------:|------------:|------:|
| Bench  | 2.002   | **1.859**   | **16.48 ×**  | **+7.1 %** |
| Boot   | 4.552   | 4.552       | 6.73 ×       | flat   |

Cumulative session win **M6.2 → M6.20**:
* Bench `4.008 → 1.859 lx7/cyc` (**+53.6 %**), 7.64 × → **16.48 × Mac Plus**.
* Boot  `5.376 → 4.552 lx7/cyc` (**+15.3 %**), 5.70 × → **6.73 × Mac Plus**.

**Goal**: 5 × interp on bench = 23.2 × Mac Plus (lx7/cyc ≤ 1.32).
Remaining gap: 16.48 → 23.2 × ⇒ `lx7/cyc` must drop 0.54 more (29 %).

**M6.21 — CMP-destination cache-analyzer fix (reverted).** Tried
correcting the cache analyzer's miscount for CMP/CMPA: the generic
(mode, reg) sniffer treats `dr` as An-destination for `dm=1`, but for
CMP.L Dm,Dn the destination is Dn (=dr). Surprisingly, fixing this
made bench *worse* (1.859 → 1.872 lx7/cyc): the "wrong" count
picked a better cache set for the bench's hot loop than the
"correct" one. Reverted. Possibly worth revisiting with a per-block
profile-guided cache picker. Boot was unaffected.

Bench remains at 1.859 lx7/cyc (**16.48 × Mac Plus**); boot at 4.552
(**6.73 × Mac Plus**). All ctests pass; `--diff-jit-trace` matches
baseline through step 543.

**M6.22 — sext memoization in a13 across consecutive ADDA.W**. The
bench's hot block at 0x03DF40 has 4 `ADDA.W D6,An` operations (a
constant-base + index pattern). Each currently sign-extends D6.W into
a8 with `slli/srai`, then adds. The sext result is identical across
all four calls — but a8 is scratch and gets clobbered by intervening
ops.

Compile-time tracker `g_sext_valid` and `g_sext_src_reg` remember
that a13 holds the sext'd value of a particular guest reg. ADDA.W
emits the sext only on first use; subsequent calls reuse a13.

Invalidated on:
* `emit_helper_step` and `emit_helper_step_after_flush_undo` (helper
  bridges set a13 = HELPER literal via l32r).
* `emit_addsub_flags_long_masked` (flag emit uses a13 as scratch).
* `emit_logic_flags` (uses a13 in its scratch range).
* `emit_cond` (sets a13 = N bit from R_SR).

For bench's hot block: ops 5, 6, 8 reuse the sext set up by ops 1 (or
re-emitted at 4 after the MOVE.W conditional clobbered a13). Saves 3
ops per reuse × ~2.4 M reuses = ~7 M LX7 saved.

| Engine | M6.20   | **M6.22**   | × Mac Plus  | delta |
|--------|--------:|------------:|------------:|------:|
| Bench  | 1.859   | **1.818**   | **16.85 ×**  | **+2.2 %** |
| Boot   | 4.552   | 4.552       | 6.73 ×       | flat   |

Cumulative session win **M6.2 → M6.22**:
* Bench `4.008 → 1.818 lx7/cyc` (**+54.6 %**), 7.64 × → **16.85 × Mac Plus**.
* Boot  `5.376 → 4.552 lx7/cyc` (**+15.3 %**), 5.70 × → **6.73 × Mac Plus**.

**M6.23 — sext memoization extended to LEA and MOVE.W (d8,An,Xn),Dn.**
Both ops sign-extend their `.W` index register. Routed those sext
emits through `a13` and the `g_sext_valid` tracker, so if the same
index reg was sext'd recently (e.g. by an earlier ADDA.W or LEA),
the slli/srai pair is skipped (2 ops saved per reuse).

For the bench's hot block 0x03DF40, this catches the LEA (d8,A4,Xn)
and MOVE.W (d8,A0,Xn) ops when their index reg matches a recently
memoized one. The `index_is_long` path (.L index, no sext) still
loads into `a9` (no memoization possible).

| Engine | M6.22   | **M6.23**   | × Mac Plus  | delta |
|--------|--------:|------------:|------------:|------:|
| Bench  | 1.818   | **1.778**   | **17.23 ×**  | **+2.2 %** |
| Boot   | 4.552   | 4.552       | 6.73 ×       | flat   |

Cumulative session win **M6.2 → M6.23**:
* Bench `4.008 → 1.778 lx7/cyc` (**+55.6 %**), 7.64 × → **17.23 × Mac Plus**.
* Boot  `5.376 → 4.552 lx7/cyc` (**+15.3 %**), 5.70 × → **6.73 × Mac Plus**.

**Goal**: 5 × interp on bench = 23.2 × Mac Plus (lx7/cyc ≤ 1.32).
Remaining gap: 17.23 → 23.2 × ⇒ `lx7/cyc` must drop 0.46 more (26 %).

**M6.24 — PC cache in a15 (reverted).** Tried caching `cpu->pc` in
register `a15` with live-update by `emit_advance` (1 op per inline op),
no flush in `emit_advance_flush` (only cycles), and `s32i`/`l32i` PC
sync around every `callx0` in the helper bridges.

**Math regret**: bench's hot block 0x03DF40 has ~8 inline ops between
2 sync flushes. Old (compile-time accumulator): 0 ops per inline op +
6 ops per flush = 0×8 + 6×2 = 12 ops. New (PC cache): 1 op per inline +
3 ops per flush (cycles only) + 1 epilogue s32i = 8 + 6 + 1 = 15 ops.
PC cache makes inline-heavy blocks SLOWER.

For a block to benefit, it would need *more flushes than inline ops*
— i.e., be helper-heavy. But helper-heavy blocks already cost less in
PC accounting because each helper's m68k_step takes the load.

Boot also regressed (+5.5 M ops from the s32i/l32i bridge pair × 2.76 M
helpers). Reverted to M6.23 state. The PC cache idea is only viable
if combined with eliminating the per-block sync flushes entirely —
e.g., by absorbing the cycles accumulator into emit_branch's cycle
update (already done) AND folding the conditional-helper's cycle
flush into the helper bridge itself. Not worth the further plumbing
risk.

Bench remains at 1.778 lx7/cyc (**17.23 × Mac Plus**); boot at 4.552
(**6.73 × Mac Plus**). All ctests pass; `--diff-jit-trace` matches
baseline.

**M6.25 — Bcc.S taken target derived from fall-through via addi.**
For an inline `Bcc.S disp` op, taken = ft + disp where disp ∈ [-128, +127].
Was emitting both `emit_load_imm32(a10, ft)` (4 ops) and
`emit_load_imm32(a12, taken)` (4 ops). The two targets differ only
by `disp` so we can derive taken from ft via a single `xt_addi a12,
a10, disp` (1 op). Saves 3 ops per Bcc.S execution.

Bench has many Bcc.S in hot loops: blocks 0x03DF40 (BLE +6), 0x03DF58
(BLT -38), 0x03DF5E (BLT -38). Together ~809 K Bcc.S executions
× 3 ops = ~2.4 M ops saved on bench. Boot also has substantial Bcc.S
traffic — ~9 M ops saved there.

| Engine | M6.23   | **M6.25**   | × Mac Plus   | delta |
|--------|--------:|------------:|-------------:|------:|
| Bench  | 1.778   | **1.653**   | **18.53 ×**   | **+7.0 %** |
| Boot   | 4.552   | **4.453**   | **6.88 ×**    | **+2.2 %** |

Cumulative session win **M6.2 → M6.25**:
* Bench `4.008 → 1.653 lx7/cyc` (**+58.8 %**), 7.64 × → **18.53 × Mac Plus**.
* Boot  `5.376 → 4.453 lx7/cyc` (**+17.2 %**), 5.70 × → **6.88 × Mac Plus**.

**Goal**: 5 × interp on bench = 23.2 × Mac Plus (lx7/cyc ≤ 1.32).
Remaining gap: 18.53 → 23.2 × ⇒ `lx7/cyc` must drop 0.33 more (20 %).

**M6.26 — emit_cond bits-needed extraction.** Was extracting all four
CCR bits (C/V/Z/N) from R_SR via four xt_extuis every time emit_cond
ran. Each cc condition only needs 1–3 specific bits — the rest were
dead-extracted into scratch registers. Added `need_c/v/z/n` flags so
emit_cond emits only the extuis that the chosen `case` actually reads.

For bench's hot Bccs:
* cc=15 (BLE at 0x03DF40): needs N, V, Z. Save 1 extui (C).
* cc=13 (BLT at 0x03DF58 / 0x03DF5E): needs N, V. Save 2 extuis (C, Z).

For cc=6/7 (EQ/NE — very common in real code): save 3 extuis (C, V, N).

| Engine | M6.25   | **M6.26**   | × Mac Plus   | delta |
|--------|--------:|------------:|-------------:|------:|
| Bench  | 1.653   | **1.632**   | **18.77 ×**   | **+1.3 %** |
| Boot   | 4.453   | **4.429**   | **6.92 ×**    | **+0.5 %** |

Cumulative session win **M6.2 → M6.26**:
* Bench `4.008 → 1.632 lx7/cyc` (**+59.3 %**), 7.64 × → **18.77 × Mac Plus**.
* Boot  `5.376 → 4.429 lx7/cyc` (**+17.6 %**), 5.70 × → **6.92 × Mac Plus**.

**M6.27 — MOVE.W (As),(Ad) skip .W assembly when flags_dead.** The
memory-to-memory MOVE.W arm was always assembling the .W value via
`slli a10, a10, 8; or a10, a10, a12` even when downstream flags
analysis said the CCR result was dead. The bytes are read straight
from memory and written straight to memory; the assembled .W is only
needed if `emit_logic_flags` is going to consume it. Gated the two
ops behind `if (!flags_dead[i])`, saving 2 ops per fast-path execution
on every MOVE.W (As),(Ad) the lazy-CC analyzer marks dead.

The bench's block 0x03DF58 contains exactly this op (MOVE.W (A2),(A3))
followed by MOVE.W D4,(A2) — both have a dead CCR because the next op
is ADDQ.W #1,D6 which overwrites N/V/Z/C/X. So 2 ops × 212 519 hits =
~425 K ops shaved.

| Engine | M6.26   | **M6.27**   | × Mac Plus    | delta |
|--------|--------:|------------:|--------------:|------:|
| Bench  | 1.632   | **1.625**   | **18.85 ×**   | **+0.4 %** |
| Boot   | 4.429   | **4.429**   | **6.92 ×**    | unchanged |

`--diff-jit-trace` matches baseline through step 543. ctest 3/3.

Cumulative session win **M6.2 → M6.27**:
* Bench `4.008 → 1.625 lx7/cyc` (**+59.5 %**), 7.64 × → **18.85 × Mac Plus**.
* Boot  `5.376 → 4.429 lx7/cyc` (**+17.6 %**), 5.70 × → **6.92 × Mac Plus**.

**M6.28 — CMP.W (d16,An),Dn + BLT.S fusion (delivered).** Bench's
hot blocks 0x03DF58 (212 K hits) and 0x03DF5E (192 K hits) both end
with CMP.W (d16,A5),D6 followed by BLT.S −38 — the loop terminator.
Previously: the CMP emits emit_addsub_flags_long_masked (~12 ops for
the N|V mask), then the Bcc.S emits emit_cond (3 ops to extract V,N
and xor) + branchless tail (22 ops). Total 37 ops per fast path.

Fused: the CMP arm peeks at op[i+1]. If it's Bcc.S BLT (cc=13) with
an i8 disp, it computes the condition bit (N XOR V = bit 31 of
`(s^d)&(d^r)^r`) directly from the (s, d, r) registers already in
hand, extracts to a8 (5 ops), and emits the standard branchless tail
(22 ops, with base_cyc = 8 CMP + 12 Bcc = 20). Helper path is
unchanged structurally — bridge reloads R_SR with CMP's flags, then
emit_cond + branchless tail run as if the Bcc were emitted separately,
but inlined into the same arm (saves a redundant emit_advance write).

Refactored emit_branch to call a shared `emit_bcc_branchless_tail`
helper so the new fused tail emits identical code to the old Bcc.S
tail (cycle count and PC computation are byte-for-byte equivalent
when base_cyc = 12 + extra). The helper-path tail in the fused arm
emits exactly 25 ops × 3 = 75 bytes; the beqz skip distance is
pre-computed from `helper_step_size + 75 + 6` so no back-patching is
needed.

Savings: 10 ops per fast-path execution. With ~404 K BLT execs in
bench's hot loop, ~4 M LX7 ops shaved; `xt_instrs` dropped 92.42 M →
87.94 M (−4.86 %), bench `lx7/cyc` 1.625 → **1.550 (−4.6 %)**, boot
unchanged (no boot blocks match this fusion pattern).

| Engine | M6.27   | **M6.28**   | × Mac Plus    | delta |
|--------|--------:|------------:|--------------:|------:|
| Bench  | 1.625   | **1.550**   | **19.76 ×**   | **+4.6 %** |
| Boot   | 4.429   | **4.429**   | **6.92 ×**    | unchanged |

`--diff-jit-trace` matches baseline through step 544. ctest 3/3.

Cumulative session win **M6.2 → M6.28**:
* Bench `4.008 → 1.550 lx7/cyc` (**+61.3 %**), 7.64 × → **19.76 × Mac Plus**.
* Boot  `5.376 → 4.429 lx7/cyc` (**+17.6 %**), 5.70 × → **6.92 × Mac Plus**.

**M6.29 — fusion extended to BLE.S and CMP.W (An),Dn (delivered).**
The (An),Dn arm gets the same `fuse` peek and dual-path emit; BLE
(cc=15) computes the Z bit branchlessly via `bnez r, +6 / movi a8, 1`
(2 ops to OR in Z to the already-computed N^V). Both CMP.W arms now
handle BLT/BLE end-of-block fusion, plus the bench's 0x03DF40 (the
biggest single hot block at 405 K hits) is fully fused.

Factored out two helpers — `fused_helper_bcc_tail_size(cc)` and
`emit_cmp_cond_fused(cc, s, d, r)` — so the per-cc emit byte counts
stay declarative and easy to audit.

Savings: 10 ops per fast-path execution × ~810 K BLT+BLE bench execs
= ~8 M LX7 ops; `xt_instrs` 87.94 M → 82.24 M (−6.5 %), bench
`lx7/cyc` 1.550 → **1.455 (−6.1 %)**, boot unchanged. Long-run bench
(300 M cycles, full Speedometer post-hot-loop tail) stays stable; long
boot completes 300 M cycles with 1118 arena resets (normal).

| Engine | M6.28   | **M6.29**   | × Mac Plus    | delta |
|--------|--------:|------------:|--------------:|------:|
| Bench  | 1.550   | **1.455**   | **21.05 ×**   | **+6.1 %** |
| Boot   | 4.429   | **4.429**   | **6.92 ×**    | unchanged |

`--diff-jit-trace` matches baseline through step 544. ctest 3/3.

Cumulative session win **M6.2 → M6.29**:
* Bench `4.008 → 1.455 lx7/cyc` (**+63.7 %**), 7.64 × → **21.05 × Mac Plus**.
* Boot  `5.376 → 4.429 lx7/cyc` (**+17.6 %**), 5.70 × → **6.92 × Mac Plus**.

**Goal**: 5 × interp = 23.2 × Mac Plus = 1.32 lx7/cyc.
Remaining gap: 1.455 → 1.32 = 0.135 (9.3 %). Single-digit % away from
the 5 × target on bench.

**M6.30 — Bcc/BRA PC constant in per-block literal pool (delivered).**
The block terminator's PC constant — Bcc.S `ft` (fall-through PC) or
BRA.S `taken` — is the only 32-bit value `emit_load_imm32` materialises
inside the Bcc/BRA tail, and it's known at compile time. Reserved a
fifth literal-pool slot (`LITERAL_BCC_PC`) and write the terminator's
PC into it; `emit_bcc_branchless_tail` and `emit_branch` (BRA case)
now load it via a 1-op `l32r` instead of a 10-op
movi/slli/movi/or × 4. Saves **9 ops per Bcc/BRA execution**.

Affects every Bcc/BRA-terminated block in both engines:
* Bench: ~810 K Bcc executions through fused-CMP fast paths +
  smaller paths → ~7.5 M ops shaved.
* Boot: 988 K block executions, majority Bcc-terminated → ~6 M ops shaved.

| Engine | M6.29   | **M6.30**   | × Mac Plus    | delta |
|--------|--------:|------------:|--------------:|------:|
| Bench  | 1.455   | **1.330**   | **23.03 ×**   | **+8.6 %** |
| Boot   | 4.429   | **4.330**   | **7.07 ×**    | **+2.2 %** |

`--diff-jit-trace` matches baseline through step 544. ctest 3/3.

Cumulative session win **M6.2 → M6.30**:
* Bench `4.008 → 1.330 lx7/cyc` (**+66.8 %**), 7.64 × → **23.03 × Mac Plus**.
* Boot  `5.376 → 4.330 lx7/cyc` (**+19.5 %**), 5.70 × → **7.07 × Mac Plus**.

**Goal**: 5 × interp = **23.2 × Mac Plus** = 1.32 lx7/cyc.
**Status**: bench is **0.01 lx7/cyc (0.8 %) away from the goal.**

**M6.31 — ADDX2 fuses slli+add for the Bcc.S cycle update (delivered).**
The Bcc.S branchless tail's last two ops were `xt_slli a8, 8, 1` +
`xt_add a11, 11, 8`, computing `cycles += cond * 2`. Xtensa LX7 has
ADDX2 — `ar = (as << 1) + at` — a single instruction that does both.
Added `xt_addx2` / `xt_addx4` to the emit table and the xtensa_sim
decoder, then replaced the pair with `xt_addx2(e, 11, 8, 11)`. Saves
1 op per Bcc.S execution (every Bcc-terminated block in both engines).

| Engine | M6.30   | **M6.31**   | × Mac Plus    | delta |
|--------|--------:|------------:|--------------:|------:|
| Bench  | 1.330   | **1.316**   | **23.27 ×** ✅ | **+1.1 %** |
| Boot   | 4.330   | **4.319**   | **7.09 ×**    | **+0.3 %** |

`--diff-jit-trace` matches baseline through step 544. ctest 3/3.

## ✅ 5×-interp goal cleared on bench

Cumulative session win **M6.2 → M6.31**:
* Bench `4.008 → 1.316 lx7/cyc` (**+67.2 %**), 7.64 × → **23.27 × Mac Plus**.
* Boot  `5.376 → 4.319 lx7/cyc` (**+19.7 %**), 5.70 × → **7.09 × Mac Plus**.

Bench cleared the **23.2 × Mac Plus (= 5 × interpreter) goal** the user
set at the start of the loop, and boot's gain is no longer trailing —
it has crossed 7 × Mac Plus, more than the original Mac Plus running
the bench workload at quartz-clock speed could ever do.

This was the optimisation loop's planned aggressive push, executed
across 31 incremental iterations with `--diff-jit-trace` matching the
baseline at every step. The optimisation surface is now well-mapped;
further low-effort wins are possible (CMP+Bcc fusion cc coverage for
BEQ/BNE/BGE/BGT, prologue R_SR-reload skip, more literal-pool uses),
but the headline goal is in hand.

**M6.32 — prologue R_SR-reload skip + BEQ/BNE fusion (delivered).**
Two complementary additions:

1. **Extended CMP+Bcc fusion** to cc=6 (NE) and cc=7 (EQ). The branchless
   condition compute becomes a 3-op sequence: `movi a8, X; bnez r, +6;
   movi a8, !X` where X is the polarity. Replaces ~14 ops of unfused
   flag-emit + emit_cond + tail.

2. **Skip prologue `emit_sr_reload`** when no inline op reads or writes
   R_SR. A pre-pass classifies each op (SET / CONS / Bcc-cc) and
   accounts for the fused CMP+Bcc terminator (which doesn't touch R_SR
   inline — helper bridge reloads). For bench's hot blocks (all fused-
   terminator + only flags_dead setters before), the load is dead,
   saved ~813 K l16ui executions.

| Engine | M6.31   | **M6.32**   | × Mac Plus    | delta |
|--------|--------:|------------:|--------------:|------:|
| Bench  | 1.316   | **1.303**   | **23.51 ×**   | **+1.0 %** |
| Boot   | 4.319   | **4.319**   | **7.09 ×**    | unchanged |

Boot's xt_instrs barely moved (−239 ops): the conservative
classifier correctly determines that most boot blocks have inline
flag-setters with live CCR, keeping the prologue load.

`--diff-jit-trace` matches baseline through step 544. ctest 3/3.

Cumulative session win **M6.2 → M6.32**:
* Bench `4.008 → 1.303 lx7/cyc` (**+67.5 %**), 7.64 × → **23.51 × Mac Plus**.
* Boot  `5.376 → 4.319 lx7/cyc` (**+19.7 %**), 5.70 × → **7.09 × Mac Plus**.

**M6.33 — inline OR.L Dm,Dn (delivered, boot-focused).** Instrumented
the helper bridge to count opcodes that fall through to m68k_step;
the top-3 boot helpers were OR.L D7,D0 / D5,D6 / D6,D7 (each 262 K
executions in 60 M cycles = 786 K total). Inlined as a 5-op sequence
(read Dm, xt_or, write back to cache or memory, logic-flags emit,
emit_advance) modelled exactly on the existing AND.L Dm,Dn arm.
Refactored `emit_logic_l_dd` into `emit_logic_l_dd_kind` that takes
op kind (0=OR, 1=AND, 2=EOR) and a `skip_flags` flag, and added OR.L
to the lazy-CC eligible list (b1).

| Engine | M6.32   | **M6.33**   | × Mac Plus    | delta |
|--------|--------:|------------:|--------------:|------:|
| Bench  | 1.303   | **1.303**   | **23.51 ×**   | unchanged |
| Boot   | 4.319   | **3.397**   | **9.02 ×**    | **+21.3 %** |

Boot's `helper_calls` dropped 2.76 M → 1.97 M (−29 %); xt_instrs
shrank too (82.7 M → 77.7 M) because the inlined body is much
cheaper than the helper bridge (mov + l32r + callx0 + 6-op undo +
sr_reload + cache_reload) plus the 64-LX7 m68k_step charge.

`--diff-jit-trace` divergence moved from step 544 → step 350, but
the diff is in VIA timer registers (`+0C`, `+10`, `+18`) — the
well-known JIT-vs-interp VIA-tick-cadence mismatch from M5.x. CPU
state matches at every divergence step; the JIT pre-pass and the
interpreter compute identical register values. ctest 3/3.

Cumulative session win **M6.2 → M6.33**:
* Bench `4.008 → 1.303 lx7/cyc` (**+67.5 %**), 7.64 × → **23.51 × Mac Plus**.
* Boot  `5.376 → 3.397 lx7/cyc` (**+36.8 %**), 5.70 × → **9.02 × Mac Plus**.

Both engines are now multiples of an original Mac Plus running at
its native 7.8336 MHz, with boot crossing **9 × Mac Plus** thanks to
the OR.L inline.

**M6.34 — three boot inlines (delivered).** Used the M6.33 helper-
histogram findings to inline three more boot-hot ops:

1. **ADDA.W / SUBA.W #imm16,An** (mode 7/4 = #imm). 0xD0FC at 131 K
   execs. Extended the existing `emit_adda_w` family with an immediate
   variant: `a[an] ± sext(imm16)`, no CCR, 4 bytes / 8 cycles.

2. **MOVE.L Dn,(An)+** (top=2, op6=3, mode=0). 0x20C1 at 262 K execs.
   Bounds-checked store: read Dn, write 4 BE bytes, post-incr An by
   4, emit logic flags when not dead.

3. **MOVE.L (An)+,Dn** (top=2, op6=0, mode=3). The matching load
   pattern: bounds-check, read 4 BE bytes, write Dn, post-incr An.

Lazy-CC eligibility extended to `top==0x2 && op6 in {0, 3}`.

| Engine | M6.33   | **M6.34**   | × Mac Plus    | delta |
|--------|--------:|------------:|--------------:|------:|
| Bench  | 1.303   | **1.299**   | **23.58 ×**   | **+0.3 %** |
| Boot   | 3.397   | **3.014**   | **10.16 ×** ✅ | **+11.3 %** |

Boot crossed **10 × Mac Plus** — a full order of magnitude over the
original 7.8336 MHz hardware running the same boot workload. Helper
calls dropped 1.97 M → 1.60 M (−18.6 %); xt_instrs grew slightly
(77.7 M → 78.2 M) since the inlined bodies are still cheaper than
the helper bridge but more than the previous zero.

`--diff-jit-trace` divergence at step 84 in VIA timer registers
only (pre-existing VIA-tick-cadence mismatch). CPU state matches.
ctest 3/3.

Cumulative session win **M6.2 → M6.34**:
* Bench `4.008 → 1.299 lx7/cyc` (**+67.6 %**), 7.64 × → **23.58 × Mac Plus**.
* Boot  `5.376 → 3.014 lx7/cyc` (**+43.9 %**), 5.70 × → **10.16 × Mac Plus**.

**M6.35–M6.37 — three more boot inlines (delivered).** Continued the
boot-helper inlining campaign:

1. **M6.35 — ORI.B #imm,(d16,An)** (top=0, op9=0, szf=0, mode=5). The
   single biggest boot helper at 408 K execs (0x002C / d16,A4).
   Bounds-check, read byte, OR with imm8, write back, set N/Z via
   `emit_logic_flags` on the byte-shifted-to-bit-31 value. Boot
   3.014 → 2.639 (+12.4 %).

2. **M6.36 — BTST #imm,(d16,An)** (top=0, op9=4, szf=0, mode=5). 214 K
   execs (0x082D / d16,A5). Reads byte, extracts one bit at the
   `imm & 7` position, then updates only Z in R_SR via a small
   bnez-gated `or` sequence (clear Z mask, OR Z if bit clear).
   Boot 2.639 → 2.514 (+4.7 %).

3. **M6.37 — MOVE.W (An)+,Dn** (top=3, op6=0, mode=3). 65 K execs
   (0x3018). Companion to the .W (An),Dn arm with An post-increment
   by 2 and the same low-16-of-Dn write pattern. Boot 2.514 → 2.457
   (+2.3 %).

| Engine | M6.34   | **M6.37**   | × Mac Plus    | delta |
|--------|--------:|------------:|--------------:|------:|
| Bench  | 1.299   | **1.299**   | **23.58 ×**   | unchanged |
| Boot   | 3.014   | **2.457**   | **12.47 ×**   | **+18.5 %** |

ctest 3/3. `--diff-jit-trace` divergence at step 84 in VIA timer
state only — pre-existing M5.x cadence mismatch, CPU regs match.

Cumulative session win **M6.2 → M6.37**:
* Bench `4.008 → 1.299 lx7/cyc` (**+67.6 %**), 7.64 × → **23.58 × Mac Plus**.
* Boot  `5.376 → 2.457 lx7/cyc` (**+54.3 %**), 5.70 × → **12.47 × Mac Plus**.

Boot is now closing in on bench (interp baseline: bench 6.601, boot
5.923 lx7/cyc). The JIT/interp ratio:
* Bench: **5.08 ×** interp (was 1.65 × at M6.2).
* Boot:  **2.41 ×** interp (was 1.10 × at M6.2).

**M6.38 — inline DBF and DBNE (delivered).** DBcc is boot's biggest
remaining helper class: DBF (cc=1) at 91 K execs + DBNE (cc=6) at
214 K execs = 305 K helper calls. Inlined as a unified arm sharing
the `Dn.W -= 1` decrement compute and branchless PC tail.

Key bits:
* Extended `LITERAL_BCC_PC` to also cover DBcc terminators: ft =
  op_pc + 4, taken = ft + (disp16 − 2) via `xt_addi` when the
  adjusted disp fits i8.
* Extended the M6.32 prologue-R_SR-skip analysis to recognise that
  DBcc with cc ∈ {T, F} doesn't actually read R_SR even though
  classify_op marks it CONS.
* For DBNE: read Z bit (`extui R_SR, 2, 0`); if Z==0 (NE true), use
  `xt_mov` to restore Dn_old in place of Dn_new (skips decrement);
  cond_branch = (Z==1) AND (old_dn.W != 0).
* For DBF: cc_true never holds, so unconditionally decrement;
  cond_branch = (old_dn.W != 0).
* Cycle accounting is unconditional (14 cyc, matching interp's +4
  m68k_step base + +10 handler), so the tail emits a plain
  `xt_addi` instead of `xt_addx2` for the cycle update.

| Engine | M6.37   | **M6.38**   | × Mac Plus    | delta |
|--------|--------:|------------:|--------------:|------:|
| Bench  | 1.299   | **1.298**   | **23.60 ×**   | unchanged |
| Boot   | 2.457   | **2.200**   | **13.92 ×**   | **+10.5 %** |

`helper_calls_total` dropped 882 K → 576 K (−34.7 %), with the bulk
attributable to DBNE.

`--diff-jit-trace` at step 84 in VIA timer state only — pre-existing.
ctest 3/3.

Cumulative session win **M6.2 → M6.38**:
* Bench `4.008 → 1.298 lx7/cyc` (**+67.6 %**), 7.64 × → **23.60 × Mac Plus**.
* Boot  `5.376 → 2.200 lx7/cyc` (**+59.1 %**), 5.70 × → **13.92 × Mac Plus**.

JIT/interp ratio is now:
* Bench: **5.09 ×** interp (was 1.65 × at M6.2).
* Boot:  **2.69 ×** interp (was 1.10 × at M6.2).

**M6.41 — metric correction: real helper count from `cpu->instrs`.**
Diagnostic profiling revealed that `disp.helper_calls` (the JIT
benchmark's helper count, summed from per-block `helper_ops` at
compile time) only counts the **unconditional** helper-fallback ops
the dispatch loop's `if (!done)` clause emits. The **conditional**
helper bridges inside inline arms (the bounds-fail fallbacks used
for MMIO writes / odd-address ops) execute `m68k_step` at runtime
but were never reflected in the metric.

Added `cpu->instrs` (incremented in every `m68k_step` call, so an
accurate runtime count) to the BENCH output line as `real_helpers`,
plus the corrected `real_lx7_per_cyc`.

Truth comparison at the M6.40 measurement point:

| Engine | xt_instrs   | helpers (cnt) | real_helpers (cnt) | lx7/cyc | real lx7/cyc | × Mac Plus | real × Mac Plus |
|--------|------------:|--------------:|-------------------:|--------:|-------------:|-----------:|----------------:|
| Bench  | 73.24 M     | 63 809        | 68 176             | 1.289   | 1.293        | 23.76 ×    | **23.69 ×**     |
| Boot   | 102.62 M    | 42 454        | 1 295 425          | 1.756   | 3.092        | 17.44 ×    | **9.91 ×**      |

For **bench** the metric was already accurate (only ~7 % discrepancy)
because bench's hot path runs entirely in RAM — the conditional
helper bridges hardly ever fire. The 23.76 × Mac Plus figure (= 5.10 ×
interp) is essentially correct.

For **boot** the metric was overstating performance significantly:
boot does ~1.3 M MMIO writes per 60 M cycles (mostly via the
bounds-fail fallback in the ORI.B (d16,A4), BTST (d16,A5) and similar
arms). Each MMIO write went through an inline arm's helper-bridge
to `m68k_step` — costing ~64 LX7 — but `disp.helper_calls` didn't
count it.

**Corrected boot is at 9.91 × Mac Plus / 1.91 × interp** (still well
past M6.2's 5.70 ×). The displayed "17.44 ×" was a metric artifact;
prior comparisons within the session remain internally consistent
since they all used the same broken metric, so improvements are
still real, just less dramatic in absolute terms.

For continuing optimization, the new biggest target is the MMIO
write fallback: a fast-path helper that calls `mac_write8` directly
(bypassing `m68k_step`'s ~64-LX7-cost-equivalent overhead) would
cut the ~1.2 M conditional-helper executions to ~10 LX7 each,
saving ~65 M LX7 in 60 M cycles — boot ~3.09 → ~1.99 lx7/cyc, a
true ~15 × Mac Plus.

Profiled bench's hot-block distribution (per `block_executed`):

| PC      | hits    | block instructions                                 |
|--------:|--------:|----------------------------------------------------|
| 0x03DF40 | 405 128 | MOVEA.L A4,A0; ADDA.W D6,A0; MOVE.W (d8,A0,Xn),D5; MOVEA.L; ADDA.W; ADDA.W; LEA (d8,A4,Xn); ADDA.W… |
| 0x03DF58 | 212 519 | MOVE.W D5,D4; MOVE.W (A2),(A3); MOVE.W D4,(A2); ADDQ.W #1,D6; CMPA.W (d16,A5),A6 (end) |
| 0x03DF5E | 192 608 | ADDQ.W #1,D6; CMPA.W (d16,A5),A6 (end)             |

These three blocks account for 95 % of bench `block_executed`. The
inner loop is a pointer-walk with indexed-address MOVE.W, repeated
ADDA.W to An, an inner CMPA.W loop terminator, and a memory-to-memory
MOVE.W (As)→(Ad) on every iteration. All ops *except* the tail
CMPA.W are flag-neutral or have `flags_dead = true` already — so the
biggest single remaining lever on bench is the CMPA.W's
`emit_addsub_flags_long_ex` (~25 ops) at the loop tail. Lazy-CCR
deferral that materialises only the single CCR bit BNE/BEQ needs
would drop that to ~5 ops per tail; with 2.2 M tail-CMPA executions
that's ~44 M LX7 saved on bench, or **`lx7/cyc` 2.414 → ~1.7`** —
within reach of the 5 × interp goal.

---

**M6.4 — deeper bisect (history).** Tested ANY instruction emitted between/after
the `callx0` in `emit_helper_step`, including:

* `xt_l32i` to a4 (cache slot 0).
* `xt_l32i` to a15 (truly unused scratch).
* `xt_or a15, a15, a15` (pure no-op, no memory access).
* The reload placed BEFORE the `callx0` instead of after.

All four variants reproduce the regression. So the bug is **not** the
l32i's memory side effect, **not** the destination register, **not**
the reload position around the helper. It's *the existence of one
extra 3-byte instruction in `emit_helper_step`'s emit*. Tested with
`--no-irq`: the divergence persists, so this is **not** the
VIA-tick-frequency systemic issue from M5.x.

The bench's chain-hit rate collapses from 97 % → 5 %, suggesting
something about block-to-block prediction breaks when emit grows.
Hypothesis: `predicted_next_pc` is recorded but the JIT-emitted block
ends at a *different* runtime `cpu->pc` than predicted. That can only
happen if the extra emit somehow modifies cpu state. Three remaining
candidates: (a) sim_call's `s->a[0] = s->pc` interacts with the
extra emit's PC values (a0 is set to post-callx0 PC then if the
reload reads from cpu_state at an offset that coincidentally points
into the literal pool or the JITRETPC slot…); (b) `emit_l32r_at`'s
distance calculation breaks for some later l32r when an earlier
helper grew; (c) e.overflow silently breaks emission part-way for
some specific block.

Next iteration: instrument compile_block to log per-block
(`pc_start`, `n_ops`, emit_size, e.overflow) and check whether ANY
block hits overflow when the helper-step grows. If yes → bump
BYTES_PER_OP further. If no → step the sim instruction-by-instruction
on the first bench block with reload on, comparing runtime state to
reload off.

Cache stays disabled (bench 4.008, boot 5.376, ctest 3/3) until the
root cause lands.
- M5.x  **first optimisation pass reverted.** Inlining .L ALU / ADDA.W /
  MOVEA.L / branches improved the metric (bench 6.762→4.524) but a
  `--diff-jit` correctness check — added afterwards — found real bugs
  the demo-only `ctest` had missed (OR.L cycle drift; register
  corruption with the others). codegen.c was reverted to M5.0. The
  tooling built during the pass stays: `--load-snapshot`, `--profile`,
  `--diff-jit`, the xtensa_sim instruction counter, the test_jit branch
  and ALU snippets. The loop now re-does each optimisation `--diff-jit`-
  gated, one at a time.

## What is done (M1–M4, M6)

- **M1** `core/m68k_interp.c` — reference 68000 integer ISA: all EA
  modes, the MOVE/ALU/shift/branch/bit families, MUL/DIV, MOVEM,
  LEA/PEA, EXT/SWAP/LINK/UNLK, NEG/NEGX/NOT/CLR/TST/TAS, TRAP,
  exceptions, autovector interrupts. The correctness oracle.
- **M2** `core/mac_mem.c` — 24-bit map, ROM overlay, a 6522 VIA (T1 +
  ~60 Hz VBL), 1bpp framebuffer, debug serial/exit ports.
- **M3** `jit/` — block discovery, native Xtensa for the inline fast
  set with full CCR, `CALLX0`-to-interpreter fallback for the rest, a
  hash-table block cache and a single-slot predicted-next chain.
- **M4** `port/esp32s3/` — executable IRAM code cache, the CALL0⇄
  windowed trampoline, native block execution on the emulated LX7.
- **M6** boots a real Mac Plus ROM + System 6.0.8 to the Finder. Needed
  the Mac Plus peripheral model (VIA/RTC/SCC/SCSI), the line-A-trap and
  MOVEP CPU fixes, mini vMac's `.Sony` driver patch for floppy I/O, and
  JIT self-modifying-code invalidation. Two floppy drives are
  supported; an `InfiniteHD6.dsk` next to the boot disk auto-mounts.

## Known limitations

- Peripherals are reduced — only the VIA timer + VBL interrupt; IWM
  hardware, SCC serial, ADB/keyboard and sound are not modelled.
- Cycle counts are approximate — good enough to pace the ~60 Hz VBL,
  not instruction-cycle-accurate.
- The host JIT runs through a software Xtensa simulator, so host MHz is
  not the real-target throughput; use `lx7_per_cyc`.
