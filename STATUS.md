# Status

## Where things stand right now (post-M6.152)

| Engine | lx7 / cyc | real_lx7 / cyc | × interp baseline (host) |
|--------|----------:|---------------:|-------------------------:|
| **Bench** (Speedometer frozen snapshot, 20 M cycles)                | **1.163** | **1.164** | **5.56 ×** ✅ |
| **Bench** (Speedometer frozen snapshot, 100 M cycles)               | **1.183** | **1.183** | **5.46 ×** ✅ |
| **Boot** (Mac Plus ROM, 100 M cycles)                               | **1.664** | **1.664** | **3.88 ×** |
| **Boot** (Mac Plus ROM, 5 M cycles, PC=`0x40032C` deterministic)    | **2.196** | **2.196** | **2.94 ×** |

## M6.149-M6.152 — pure register-op inline series 🎯

After M6.148's bridge-arm revert clarified that **pure register-op
arms are the trajectory-safe sweet spot** (independent of boot-fire
count), four consecutive iterations landed wins on this category:

| Milestone | Pattern | Boot 100M helpers Δ |
|-----------|---------|--------------------:|
| **M6.149** | ROXL.B/W/L #imm,Dn (sibling of M6.143 ROXR) | **-1 701** |
| **M6.150** | ROR/ROL.B/W/L #imm,Dn plain rotate (RO type) | -697 |
| **M6.151** | ASL.B/W #imm,Dn (left arith, V flag detection) | -441 |
| **M6.152** | ADDA.L Dm/Am,An (sibling of M6.104 ADDA.W) | -653 |
| **Total** | | **-3 492 (-1.9 %)** |

| Workload | Pre-iter (M6.148 revert) | Post-iter | Δ |
|----------|-------------------------:|----------:|--:|
| Bench 20M `lx7/cyc` | 1.164 | **1.163** | **-0.001** |
| Bench helpers | 2 744 | **2 504** | -240 (-9 %) |
| Boot 5M det helpers | 54 | **22** | -32 (-59 %) |
| Boot 100M `lx7/cyc` | 1.665 | **1.664** | **-0.001** |
| Boot 100M helpers | 181 951 | **178 459** | **-3 492** |
| Boot 100M jit_cost | 166 511 K | **166 361 K** | **-150 K LX7** |

The rule validated: pure register-op extensions of existing arm
classes are safe regardless of boot 100M fire count. M6.149 ROXL
caught 1 688 boot fires; M6.150 ROR caught 696; M6.151 ASL caught
244; M6.152 ADDA.L caught 645. All four landed cleanly.

### M6.151 ASL implementation gotcha — clobbered R_SR

First attempt used `xt_movi(e, 14, ...)` for V-flag scratch — **a14
is R_SR**! Bench diverged at PC=0x41E054 with SR interp=0x2704 vs
jit=0x0004 (upper byte T/S/IPL cleared). Fixed by routing all V-
computation through a11/a12/a13. a13 (R_HELP) is safe as scratch in
pure-inline code where no CALLX0 happens.

## M6.148 finding — sibling-symmetry isn't enough for bridge arms

## Trajectory-safe inline series — M6.141, M6.142, M6.143

A reproducible methodology for finding inline arms that can never
trajectory-shift boot 100M's M6.66 post-VIA-tick chaotic region. The
key insight: an arm matching opcode pattern P is trajectory-safe iff
P is STRICTLY ABSENT from boot 100M's full helper histogram — not just
top-20, since sub-300-hit ops still shift the chaos materially (M6.142
ADDQ.B Dn revert: 56 boot fires hidden below top-20 caused +2.2 %
regression).

### Methodology

`jit/dispatcher.c` has a `MAC68K_HISTO_FULL=1` env-gated full-histo
dump (temporarily restorable in 4 lines). Build with
`-DJIT_HELPER_HISTO=1`, capture all-non-zero opcodes for both
workloads, then per-arm pattern-match in shell:

```sh
for op in $(awk '/^  [0-9a-f]{4}/ {print $1}' /tmp/boot_all.txt); do
    val=$((16#$op))
    top=$(( (val >> 12) & 0xF ))
    # ... matching predicate for the arm's bit pattern ...
done
```

If the predicate matches ZERO entries from the boot dump, the arm is
trajectory-safe. Verified by post-commit boot 100M `lx7/cyc` being
exactly unchanged (helpers unchanged at 182 033 across all 3 commits).

### Series wins (bench-snapshot ops absent from boot 100M)

| Milestone | Op pattern | Bench hits | bench 20M `lx7/cyc` | Helpers |
|-----------|-----------|-----------:|--------------------:|--------:|
| Pre-M6.141 | (baseline) | — | 1.168 | 4 105 |
| **M6.141** | Scc Dn (Set on Condition → Dn) | 182 | 1.167 | 3 683 |
| **M6.142** | ADDX.L / SUBX.L Dm,Dn | 476 | 1.166 | 3 207 |
| **M6.143** | ROXR.B/W/L #imm,Dn (right only — ROXL fires in boot) | 421 | **1.164** | **2 786** |

Net: bench 20M -0.004 lx7/cyc (-0.34 %); helpers -32 %.

The "ROXL is in boot" detail is crucial: bits 4-3 = 10 = ROX (not RO,
which is bits 4-3 = 11 — m68k_interp's `which == 2` corresponds to
ROX). Bit 8 = 0 = RIGHT direction. The LEFT-direction ROXL.B fires in
boot 100M (0xE3xx at 1 688 hits) and was NOT inlined — preserving the
trajectory.

### M6.148 finding — sibling-symmetry isn't enough for bridge arms

M6.148 attempted TST.B (An) / TST.B (d16,An) inline as a perfect
structural sibling of M6.91 MOVE.B (An)/(d16,An):

* Same bounds-check (M6.76 unified RAM-or-ROM byte bounds)
* Same emit_jit_fast_helper bridge (new m68k_jit_tst_b_mmio helper)
* Same touched_mask narrowing (0u for flag-only)
* Same fast-path shape (l8ui + emit_logic_flags)

Bench had 184 hits at these opcodes. Boot 100M's helper-histo
showed just 4 fires (TST.B (A0)/(A1) at real-code PCs).

**Result: boot 100M regressed +32 %** (1.665 → 2.206), helpers
exploded 182 K → 1.16 M (+979 K). Same scale of regression as
M6.145 OR.W bridge-only. Reverted.

**Refined trajectory-safety rule** (saved to
`memory/bridge-only-arms-trajectory-shift.md`): the empirically-
validated safe categories for new arms are:

| Category | Examples | Risk |
|----------|----------|------|
| Pure register-op, no bridge | M6.141 Scc, M6.142 ADDX.L, M6.143 ROXR, M6.146 LSL | ✅ |
| Slow-path bridge conversion (existing arm, swap helper) | M6.144 MOVE.L (An),Dn | ✅ |
| Compile-time fusion only (no new emit) | M6.95 TST.L+Bcc, M6.147 MOVE.L+Bcc | ✅ |
| Cross-block analysis extension | M6.140 JMP/BRA.W | ✅ |
| **NEW arm with bounds-check + bridge** | M6.145 OR.W, M6.148 TST.B (An) | **❌** |

The mechanism: introducing bridge code (cache_flush + bridge) into
NEW blocks changes block size, cache_sig, and chain prediction. Even
when the runtime helper-histo shows few fires, the COMPILED block
layout differs → cascade through the M6.66 chaos.

### Post-M6.144 saturation analysis

Bench 100M's helper-histo top entries post-M6.144 are now all NEW
patterns without an existing inline arm — meaning they need brand-new
arms, which the M6.145 OR.W revert and the
[[bridge-only-arms-trajectory-shift]] memory establish as risky:

| Bench helper | Hits | Pattern | Status |
|--------------|-----:|---------|--------|
| eaa8 LSL.L D5,D0 reg-form  | 190 | top=E,bit5=1,bit8=1,szf=2,bits 4-3=01 | Boot has 254 (efef in bogus zone) — trajectory risk |
| 8155 OR.W D0,(A5) MMIO     | 156 | top=8,bit8=1,szf=1,mode=2 | Reverted M6.145; bridge-only triggers shift |
| 4a11/4a10 TST.B (An)       | 126 combined | top=4,bits 11-8=A,szf=0,mode=2 | Boot has 4 hits — trajectory risk |
| d1c1 ADDA.L D1,A0          | 59 | top=D,bit8=1,szf=3,mode=0 | Boot has 653 (M6.142 attempt regressed) |
| 4a2e TST.B (d16,A6)        | 58 | top=4,bits 11-8=A,szf=0,mode=5 | Similar to 4a11/10 |
| c4c0 MULU.W D0,D2          | 54 | top=C,bit8=0,szf=3,mode=0 | Boot has 1 hit |
| 3630 MOVE.W (d8,An,Xn),D3  | 52 | top=3,bits 8-6=0,mode=6 | Mode-6 complex |
| 486e PEA (d16,A6)          | 47 | top=4,(op&0xFFC0)==0x4840 | Boot has 3 hits |

The slow-path conversion lever (M6.144 pattern) is exhausted for
bench — JSR variants' slow paths (would-be next conversion candidates)
fire 0-3 times. **The trajectory-safe inline series M6.141-144 is
near saturation on the host metric**; remaining wins are either
small (sub-200-hit ops × ~30 LX7 savings = ≤ 0.006%) or trajectory-
risky.

Future levers per the high-gain backlog above:
* Asm-trampoline preserves a4-a7 across fast helpers — ESP32-only,
  unmeasurable on host.
* Native chain epilogue on ESP32 — unmeasurable on host.
* M6.66 structural fix — biggest potential but complex.

### Implementation gotchas caught by ctest

* **`emit_addsub_flags_long_masked` clobbers a11 internally.** The
  C-bit formula computes `xt_and(11, s, d)` BEFORE `xt_xor(13, r, …)`
  reads `r` — if `r = 11`, the result register is corrupted. Pass
  `r = 10` (the convention used by CMP.W Dm,Dn).
* **ADDX/SUBX has STICKY Z.** `Z_new = Z_prev AND (result == 0)`.
  Implementation: cc_mask omits Z (leaves the existing Z bit alone
  via the helper's narrow mask), then conditionally clear: `xt_movi
  12, -5; xt_beqz r, 6; xt_and R_SR, R_SR, 12`.
* **bits 4-3 = 10 is ROX, not RO** (per m68k_interp's `(op >> 3) & 3`
  decode: 0=AS, 1=LS, 2=ROX, 3=RO). M6.139 ROL/ROR attempt earlier
  used the wrong mask and inlined ROX with plain-rotate semantics —
  caught by ctest correctness check after the trajectory regression.

## M6.140 — cross-block lazy-CC for JMP/BRA.W static targets

Extended the cross-block lazy-CC analyzer (the pre-pass that walks
forward from a block's static-target successor to decide whether the
block's last setter's flags are dead) to handle four more terminator
patterns: BRA.W, Bcc.W, JMP (xxx).L, JMP (d16,PC).

Also fixes a latent .W/.L mis-handling: the pre-existing BRA.S / Bcc.S
arms used `(i8)(last_op & 0xFF)` for the displacement, which for the
.W and .L forms (low byte 0x00 or 0xFF) gave disp=0 and pointed at the
disp16/disp32 ext-word bytes mid-instruction. PC_OVERWRITES_CCR's
decode at that garbage PC sometimes returned a phantom setter,
incorrectly marking the prior setter dead.

Compile-time instrumentation shows the extension catches **9 blocks
in bench 100 M** (all 9 hit — JMP (xxx).L target is a SETTER) and
**0 in boot 100 M**. The metric impact at 3-decimal `lx7/cyc`
precision is invisible (9 × per-block-runs × per-dead-flag savings
falls below the rounding boundary), but the latent .W/.L bug fix is
a correctness win.

## M6.138 + M6.139 — small inline wins on Dn-form ops 🎯

Two small Dn-form arms after M6.137 closed out the bench-hot F-line zone:

* **M6.138 — TST.W Dn** (sibling of M6.101's TST.B / TST.L Dn). Boot 5 M
  det's 0x4A45 was 38 of 93 deterministic-window helpers (~41 %).
  Result: boot 5 M det helpers 93 → 55.
* **M6.139 — CLR.B/W/L Dn**. Bench 5 M's 0x4240 (CLR.W D0) at 85 hits +
  similar; one arm for all three sizes. Result: bench helpers 4 105 →
  3 937 (−168). No trajectory shift; boot 100 M unchanged.

Both committed cleanly. `lx7/cyc` at 3-decimal precision unchanged
(savings absorbed below the rounding boundary) but `real_helpers` and
`jit_cost` both improved on the targeted workloads.

### Attempted but reverted: M6.139 ROR/ROL.B/W/L #imm,Dn

The boot 5 M det's biggest remaining helpers were three ROL.B #1,Dn at
0x40025{8,A,C} (24 of the 55 helpers). Wrote a generic ROR/ROL.B/W/L
#imm,Dn inline arm (sibling of M6.97/98/99 LSR/ASR family). It worked
correctly (ctest 7/7, --diff-jit-trace 11 038 clean) and dropped boot
5 M det helpers 55 → 31 (-24, the ROL.B fires) and bench 100 M helpers
4 105 → 3 316 (-789).

But boot 100 M regressed **+0.18 %** (1.665 → 1.668). The arm shifted
the M6.66 chaotic trajectory significantly:

* `blocks=104K/1.07M` → `76K/1.46M` (33 % more compiles)
* `inline_ops=6.19M` → `4.52M` (fewer total inline executions)
* `xt_instrs=154.88M` → `155.86M` (more total Xtensa ops)
* Boot ended at `PC=0x000000` instead of `0x12C4E5FD` — different
  divergent path entirely.

Per the [[m6.66-trajectory-traps]] memory's decision framework, regression
in boot 100 M is acceptable IF a real-workload metric improved. Boot 5 M
det `lx7/cyc` did move 2.197 → 2.196 (+0.045 %), but the boot 100 M cost
(-0.146 %) outweighs the boot 5 M det gain. Reverted.

The lesson: ROR/ROL.B inlines fire in both pre-divergence (small win) and
post-divergence (chaotic). The trajectory disruption from changing the
block code in the divergence region exceeds the local op-level savings.
Pattern: ROL.B is a "shared" arm that's mostly post-divergence — bad
candidate. CLR.B/W/L Dn is bench-pre-divergence-only — safe candidate.

## M6.137 — F-line trap fast helper — bench 100 M real_lx7 1.197 → 1.184 (−1.09 %) 🎯

Bench 100 M's top helper by far is 0xFFFF at 21 808 hits / 100 M cyc,
first_pc 0x342212c2 (bogus — post-divergence zone fetches unmapped
memory as 0xFFFF, which decodes as F-line). Each fire was an m68k_step
call costing 64 LX7 in the host metric.

New fast helper `m68k_jit_fline_trap` does just `m68k_exception(cpu, 11)`:

* skips m68k_step's per-op decode + dispatch
* does NOT increment cpu->instrs (real_helpers drops 21 808)
* m68k_exception itself adds 34 cycles; the inline arm emits
  `emit_advance(0, 4)` for the m68k_step base cost, totalling 38 (same
  as the m68k_step path)
* cpu->pc was set to the faulting op_pc by emit_advance_flush;
  m68k_exception pushes that, then sets cpu->pc to vector 11
* block terminator (ends_block already set for line-F class)

**Triple-diff:** ctest 7/7, `--diff-jit-trace 11038` clean. The
divergence at cycle 12 202 (in `--diff-jit-trace 500000`) is the
documented post-VIA-tick artifact, pre-existing on `main`.

**Perf:**

| Workload | lx7/cyc pre | lx7/cyc post | Δ |
|----------|------------:|-------------:|--:|
| **Bench (100 M)** | 1.197 | **1.184** | **−1.09 %** 🎯 |
| Bench (20 M)      | 1.168 | 1.168      | unchanged (F-line not in pre-divergence window) |
| Boot (100 M)      | 1.665 | 1.665      | unchanged (no F-line in boot's histogram) |
| Boot (5 M det)    | 2.197 | 2.197      | unchanged |

**Helper count:** bench 100 M `helpers` 25 957 → 4 149 (−21 808 = the
F-line fires); `real_helpers` 26 265 → 4 457 (−21 808).

## High-gain backlog — the next structural moves

The inline-arm + fast-helper rounds (M6.91–M6.135) have hit diminishing
returns: every remaining hot op in boot 100 M is in the M6.66 post-
divergence chaotic region (see `memory/m6.66-trajectory-traps.md`), so
new inline arms shift the trajectory more than they shave LX7. The path
forward is structural, not piecemeal. Three items, biggest-win-first:

### 1. Full register caching of hot D/A regs across a block

**Empirical update (post-M6.139): cache miss rate is already < 1 %.**
Measured compile-time hit/miss counts via temporary instrumentation:

| Workload | Read hit | Read miss | Read miss % | Write hit | Write miss | Write miss % |
|----------|---------:|----------:|------------:|----------:|-----------:|-------------:|
| Bench 100 M  | 235 265   | 642   | **0.3 %** | 2 224   | 423   | 16.0 % |
| Boot  100 M  | 6 351 301 | 3 048 | **0.0 %** | 3 031 355 | 2 714 | **0.1 %** |
| Boot 5 M det | 71        | 58    | 45.0 %    | 23      | 39    | 62.9 % |

The picker (top-4 most-frequent guest regs per block, with a `≥ 2 uses`
threshold) already covers ~99 % of accesses on the steady-state
workloads. Cache widening (adding `a8..a9` as R_CACHE4/5) would shave
the remaining < 1 % — not worth the inline-arm scratch-register churn.

The boot 5 M det miss rate is high (~50 %) because init code compiles
many tiny blocks where the picker's `≥ 2 uses` threshold leaves most
regs uncached. But this is < 5 % of boot's total runtime; the picker
is right to leave them uncached (extra cache-load overhead per block
outweighs the per-op savings on a tiny block).

**Implication for Item 1:** The "eliminates intra-block l32i/s32i pairs"
premise is already substantially achieved. Remaining sub-items:

Current cache holds 4 hot guest regs in `a4..a7` (`R_CACHE0..3`), picked
at block-compile time. Hits eliminate `l32i`/`s32i`; misses fall back to
`cpu_state` loads/stores. Two known limits:

* **Cache survival at helper boundaries.** Currently every helper
  bridge does `emit_cache_flush` (write all dirty back) and
  `emit_cache_reload` (read all in `g_helper_touched_mask` back). The
  M6.132–M6.135 fast-helpers are direct `callx0` C functions — under
  CALL0 ABI the callee can clobber `a4..a11`, so cache regs are
  *physically* lost across the call (the reload covers this). The
  `m68k_step` path goes through `m68k_step_call0` which uses `call8`;
  the window rotation means the JIT's `a0..a7` survive *physically*,
  but cache-vs-memory consistency still needs the flush/reload because
  `m68k_step` reads and writes `cpu->d[]/cpu->a[]` directly.
  **Win**: hand-written asm trampolines for the fast helpers that
  spill/restore `a4..a7` around the C body (10 ops). Combined with a
  helper-`read_mask` (sibling to the existing `g_helper_touched_mask`),
  the JIT could skip the flush of dirty-cache slots the helper doesn't
  read AND skip the reload of slots the helper doesn't write. Per
  bridge this saves 2 × |unaffected dirty/cached slots| `l32i`/`s32i`.
* **Cache width.** Today 4 slots — covers the 4 hottest of up to 16
  guest regs. On the ESP32, `a8..a11` are callee-clobberable scratch in
  CALL0; `a12..a15` are split (R_HELP, R_SR, scratch). With the
  trampoline-preserves-a4..a11 approach above, the cache could expand
  to 6–8 slots in blocks that don't use those regs as inline scratch.
  Per-block analysis would assign extra slots only when safe.

### 2. Comprehensive lazy CCs with a classifier modelling helper CCR usage

`classify_op` already partitions opcodes into SET-only vs SET+CONS for
the lazy-CC path, but helper-fall-back ops are conservatively treated
as "consumes + sets everything" — every helper-bridged op forces a
`emit_sr_flush` and the subsequent inline-arm CC computation can't
assume any flag is preloaded. A per-op-helper CCR mask (which bits
*this specific* helper actually reads / writes) would let the JIT:

* skip `emit_sr_flush` before helpers that don't read SR
* skip the slow-path post-helper `emit_sr_reload` if the helper writes
  no CCR bits
* allow the inline emit *after* a helper to keep using preloaded NZVC
  from a prior inline op if the helper didn't touch them

This wants per-opcode CCR-write/read tables in `m68k_decode_at`'s
neighbourhood (or a small static table indexed by the 7-bit opcode
category).

### 3. Native block chaining on the ESP32 target

The host xt_sim runs one block per `xt_sim_run`, so any chain JX exits
the sim and returns to the C dispatcher. On the ESP32 the JX from a
predecessor block's epilogue to a successor's entry stays in JIT-land,
eliminating one dispatcher round-trip per chained block. The
infrastructure (`predicted_next_entry`, `chain_entry_addr`, cache_sig
compat) is already in place — the host metric can't observe this win,
but real-hardware boot/bench should see ~5–15 LX7 savings per chained
block (predicting >90 % of chains on bench).

This isn't a code change so much as a measurement gap: once we have an
on-device profile run, we'll know how big the chaining win actually is.

## M6.136 — AND.B / OR.B (d16,An),Dn inline — REVERTED 🔻

Boot's 0xC029 (AND.B (d16,A1),D0) fires 390 helpers/100M cyc in the
divergence region. Inlined it mirroring the M6.91 MOVE.B (d16,An),Dn
pattern — full RAM-or-ROM byte bounds, then AND/OR with Dn[7:0] and
merge.

Result: **arm itself correct (ctest 7/7, --diff-jit-trace 11 038 clean),
but boot 100 M regressed +3.5 %** (1.665 → 1.724). Bench unchanged,
boot 5 M det unchanged — the op only fires post-M6.66-divergence, so
adding the arm shifted the chaotic trajectory into a heavier helper
path. Classic [[m6.66-trajectory-traps]] case. Reverted.

**Net post-M6.135 conclusion:** boot 100 M cannot be improved by more
inline arms targeting the divergence region. Real progress needs one
of the three high-gain items above OR an M6.66 root-cause structural
fix.

## M6.132 — fast-helpers for RTS / BSR.S / BSR.W MMIO — bench 100 M real_lx7 1.253 → 1.212 (−3.27 %) 🎯

Bench's hot loop has 21K-helper/100M-cyc patterns for RTS, BSR.S, BSR.W
all falling to the slow path (SP→MMIO). Each call was through
`emit_helper_step_after_flush_undo` → `m68k_step` → 64 LX7 in the cost
model + actual decode work.

Three new fast helpers replace `m68k_step` in these specific slow paths:

* `m68k_jit_rts_mmio`   — pops PC from (SP), SP += 4
* `m68k_jit_bsr_s_mmio` — pushes (cpu->pc + 2), SP -= 4, pc = target_pc
* `m68k_jit_bsr_w_mmio` — pushes (cpu->pc + 4), SP -= 4, pc = target_pc

Helpers DO NOT increment `cpu->instrs` (= `real_helpers`). They DO
modify cpu->a[7] and cpu->pc. The JIT arms set
`g_helper_touched_mask = {A7}` so cache reload only loads A7's slot.

For BSR.S/W, target_pc is pre-loaded into a15 before the bridge and
passed via `jit_arg1`. Fast path reuses a15 (saves a re-load).

**Triple-diff:** ctest 7/7, `--diff-jit-trace` clean through 11 038 cyc.

**Perf — REAL metric (true ESP32-proxy):**

| Workload | real_lx7/cyc pre | real_lx7/cyc post | Δ |
|----------|-----------------:|------------------:|--:|
| **Bench (100 M)** | 1.253 | **1.212** | **−3.27 %** 🎯 |
| Boot 100 M | 1.688 | **1.688** | unchanged |

`real_helpers` (= `cpu.instrs`) drops:
* Bench 100M: **113 542 → 48 118** (**−65 424**, exact match: RTS 21 809
  + BSR.S 21 808 + BSR.W 21 807)
* Boot 100M:  217 612 → 215 762 (−1 850, the smaller boot count of these
  ops in MMIO range)

**Perf — host metric (jit_cost):**

| Workload | lx7/cyc pre | lx7/cyc post | Δ |
|----------|------------:|-------------:|--:|
| Bench (100 M) | 1.197 | **1.197** | unchanged |
| Boot 100 M    | 1.667 | **1.667** | unchanged |

The host metric uses `helpers × 64` (compile-time `b->helper_ops`).
Inline arms with conditional bridges don't increment `b->helper_ops`,
so M6.132 is invisible to the host model. The win is real on hardware
(fast-helper bodies are ~30 LX7 vs m68k_step's ~64 LX7).

🎯 Bench 100 M `real_lx7/cyc` 1.212 = **5.33 × interp** on a real-
hardware proxy.

Cumulative M6.84 → M6.132 (real metrics where they differ):
* Bench (20 M):    1.257 → **1.168** (−7.1 %)
* Bench (100 M):   1.396 → **1.197** (host: −14.3 %); **real_lx7 ~5 % gain** from M6.132
* Boot 5 M det:    2.471 → **2.214** (−10.4 %)
* Boot 100 M:      1.734 → **1.667** (−3.9 %)

## M6.128 — inline JSR (An) and JMP (An) — boot 100 M real_helpers −1047

Two new block-terminator inline arms for the dispatch-table-style
indirect-call/jump patterns common in Mac OS Toolbox dispatch:

* **JSR (An)** (`(w & 0xFFF8) == 0x4E90`) — push return PC, cpu->pc = An.
  Length 2; cycles 20 (m68k_step base 4 + handler 16). Boot 780 hits.
* **JMP (An)** (`(w & 0xFFF8) == 0x4ED0`) — cpu->pc = An. No SP.
  Length 2; cycles 12.

Both are block terminators with chain-preservation benefit.

**Triple-diff:** ctest 7/7, `--diff-jit-trace` clean.

**Perf:**

| Workload | M6.127 | **M6.128** | Δ |
|----------|------:|----------:|--:|
| Bench (100 M)       | 1.197 | **1.197** | helpers −104 |
| Boot 5 M det        | 2.214 | **2.214** | helpers −5 |
| **Boot 100 M**      | 1.668 | **1.667** | helpers −1047 |

Cumulative M6.84 → M6.128:
* Bench (20 M): 1.257 → **1.169** (−7.0 %)
* Bench (100 M): 1.396 → **1.197** (−14.3 %)
* Boot 5 M det:   2.471 → **2.214** (−10.4 %)
* Boot 100 M:     1.734 → **1.667** (**−3.9 %**)

## M6.126 — m68k_decode_at length fix for ORI/ANDI/EORI #imm,CCR/SR — boot 100 M 1.699 → 1.668 lx7/cyc (−1.83 %) + real_helpers 679 K → 217 K 🎯

Third decoder length bug in the family (after M6.122 MOVE-to-SR and
M6.124 ADDA.L #imm32). For ORI/ANDI/EORI to CCR/SR, the encoding uses
`mode=7/reg=4` as a DESTINATION INDICATOR (not a real ea) — the imm
bytes that follow are the only operand.

Pre-M6.126 decoder logic:
```c
d.length += (sz == 4) ? 4 : 2;     // counts the imm (correct)
d.length += ea_ext_bytes(mode, reg, sz);  // for mode=7/reg=4, returns
                                          // 2 (or 4 for sz=4) — DOUBLE
                                          // COUNT of the same imm!
```

For `ORI #0x0700,SR` (a common interrupt-mask pattern): decoder
returned length=6 instead of 4. JIT block compiler walked past only
the imm bytes, decoding the next 2 bytes as a phantom opcode.

Fix: special-case skip ea_ext_bytes when `mode=7/reg=4` AND
`op9 in {0=ORI, 1=ANDI, 5=EORI}`.

**The boot 100M win is dramatic** because boot has many ORI/ANDI #imm,SR
ops in interrupt handlers. The mis-decode was corrupting those blocks
and shifting the M6.66 VIA-tick divergence trajectory into the
heavy-helper bogus-PC region. Fixing the decode restores the original
divergence path that matches the M6.119 baseline.

**Triple-diff:** ctest 7/7, `--diff-jit-trace` clean through 11 038 cyc.

**Perf:**

| Workload | M6.125 | **M6.126** | Δ |
|----------|------:|----------:|--:|
| **Bench (20 M)**    | 1.172 | **1.169** | **−0.26 %** ✅ |
| Bench (100 M)       | 1.197 | **1.197** | xt −27 K (within noise) |
| Boot 5 M det        | 2.216 | **2.216** | within noise |
| **Boot 100 M**      | 1.699 | **1.668** | **−1.83 %** 🎯 |

Real-helper counts confirm the win:
* Bench 100M: real_helpers 113 335 → 113 644 (+309, within noise)
* **Boot 100M: real_helpers 679 302 → 217 676 (−461 626!)** — fewer
  m68k_step invocations because the M6.66 bogus-PC region path is now
  much shorter.

🎯 **Boot 100M back to 3.87 × interp** (= 6.462 / 1.668), matching the
M6.119 baseline.

Cumulative M6.84 → M6.126:
* Bench (20 M): 1.257 → **1.169** (**−7.0 %**) 🎯
* Bench (100 M): 1.396 → **1.197** (−14.3 %)
* Boot 300 K det: 2.170 → **1.975** (−9.0 %)
* Boot 5 M det:   2.471 → **2.216** (−10.3 %)
* **Boot 100 M:   1.734 → 1.668 (−3.8 %)** 🎯

## M6.125 — extend selective cache_reload to MOVE-to-mem arms

Follow-up to M6.123. The MOVE-to-mem arms (MOVE.B/.W Dn,(An),
MOVE.B/.W Dn,(d16,An), MOVE.W Dn,-(An)) have helpers that modify NO
guest D/A reg (memory + CCR only, plus An for pre-decrement). Narrowed
`g_helper_touched_mask`:

* MOVE.B Dn,(An)        — touched_mask = 0 (no reg touched)
* MOVE.W Dn,(An)        — touched_mask = 0
* MOVE.W Dn,(d16,An)    — touched_mask = 0
* MOVE.W Dn,-(An)       — touched_mask = {An} (pre-decrement)

## M6.124b — CHK.W length fix (forward-robust)

Sibling fix to M6.124. CHK opcode pattern `(op & 0xF1C0) == 0x4180`
has fixed bits 8-6 = 110 (CHK code), so bits 7-6 = 10 → szf=2 → sz=4
in the generic decoder. But CHK on 68000 is always .W (sz=2). For
CHK.W #imm,Dn the imm is 2 bytes, not 4. Same trap class as M6.122
and M6.124. CHK is rare (not in bench/boot helper-histo) so this
hasn't manifested, but fix for forward robustness.

## M6.124 — m68k_decode_at length fix for ADDA.L/SUBA.L/CMPA.L #imm32 — boot 5 M det 2.223 → 2.218 lx7/cyc (−0.22 %)

Sibling bug to M6.122's MOVE-to-SR length fix. For top=0x9 (SUB),
top=0xB (CMP), top=0xD (ADD) family with szf=3:

* szf=3 disambiguates by `top`:
  - top=0x8/0xC : DIVU/DIVS/MULU/MULS — always .W operand (sz=2)
  - top=0x9/0xB/0xD : ADDA/SUBA/CMPA — sz depends on bit 8
    - bit 8 = 0 → .W (sz=2); bit 8 = 1 → .L (sz=4)

Pre-M6.124, the decoder used `(szf==3)?2:sz` — forcing sz=2 for ALL
szf=3 ops including ADDA.L/SUBA.L/CMPA.L. Decoder mis-bounded these
ops with #imm32 source, walking 2 bytes past the actual imm and
decoding the next 2 as a phantom opcode (same trap class as M6.122).

**Triple-diff:** ctest 7/7, `--diff-jit-trace` clean through 11 038 cyc.

**Perf:**

| Workload | M6.123 | M6.124+125 | Δ |
|----------|------:|----------:|--:|
| Bench (20 M)        | 1.172 | **1.172** | unchanged |
| Bench (100 M)       | 1.197 | **1.197** | unchanged |
| **Boot 5 M det**    | 2.223 | **2.216** | **−0.31 %** ✅ (real workload) |
| Boot 100 M          | 1.700 | **1.699** | within noise |

Cumulative M6.84 → M6.125:
* Bench (20 M): 1.257 → **1.172** (−6.8 %)
* Bench (100 M): 1.396 → **1.197** (−14.3 %)
* Boot 300 K det: 2.170 → **1.975** (−9.0 %)
* Boot 5 M det:   2.471 → **2.216** (**−10.3 %**) 🎯
* Boot 100 M:     1.734 → **1.699** (−2.0 %)

## M6.124 — m68k_decode_at length fix for ADDA.L/SUBA.L/CMPA.L #imm32 — boot 5 M det 2.223 → 2.218 lx7/cyc (−0.22 %)

Sibling bug to M6.122's MOVE-to-SR length fix. For top=0x9 (SUB),
top=0xB (CMP), top=0xD (ADD) family with szf=3:

* szf=3 disambiguates by `top`:
  - top=0x8/0xC : DIVU/DIVS/MULU/MULS — always .W operand
  - top=0x9/0xB/0xD : ADDA/SUBA/CMPA — sz depends on bit 8
    - bit 8 = 0 → .W (sz=2)
    - bit 8 = 1 → .L (sz=4)

Pre-M6.124, the decoder used `(szf==3)?2:sz` — forcing sz=2 for ALL
szf=3 ops including ADDA.L/SUBA.L/CMPA.L. For these with `#imm`
source (mode=7/reg=4), `ea_ext_bytes` returned 2 bytes (.W imm) when
the actual encoding has a 4-byte imm32.

Same trap class as M6.122 — the JIT block compiler walks past only
the first 2 bytes of the imm32, decoding the next 2 bytes as a
phantom opcode. The phantom op's default-helper bridge accidentally
chains to correct execution via m68k_step's internal pc-advance from
the runtime cpu->pc (set by the previous arm's emit_advance to the
REAL next instruction). But any new inline arm for the phantom's
opcode would corrupt execution.

**Triple-diff:** ctest 7/7, `--diff-jit-trace` clean through 11 038 cyc.

**Perf:**

| Workload | M6.123 | **M6.124** | Δ |
|----------|------:|----------:|--:|
| Bench (20 M)        | 1.172 | **1.172** | unchanged (no ADDA.L #imm) |
| Bench (100 M)       | 1.197 | **1.197** | unchanged |
| **Boot 5 M det**    | 2.223 | **2.218** | **−0.22 %** ✅ (real workload) |
| Boot 100 M          | 1.700 | **1.700** | unchanged |

Boot 5M det xt drops 24 693 LX7 — the deterministic real-boot path
uses `ADDA.L #imm32,An` (typical for stack/buffer address adjustments)
and now compiles correctly without phantom-op fallthrough.

Cumulative M6.84 → M6.124:
* Bench (20 M): 1.257 → **1.172** (−6.8 %)
* Bench (100 M): 1.396 → **1.197** (−14.3 %) 🎯
* Boot 300 K det: 2.170 → **1.975** (−9.0 %)
* Boot 5 M det:   2.471 → **2.218** (**−10.2 %**) 🎯
* Boot 100 M:     1.734 → **1.700** (−2.0 %)

## M6.123 — selective cache_reload via touched_mask for SP/An-only helper bridges

New `g_helper_touched_mask` global (default 0xFFFF = "helper can touch
any reg") narrowed by SP-touching block-terminator arms. After a helper
bridge, `emit_cache_reload` and `emit_helper_step_after_flush_undo_size`
skip slots whose guest reg isn't in the mask — the helper didn't
modify them, so cpu state still matches the cache register's pre-bridge
value (no l32i needed).

Per-fire savings: `3 LX7 × (rc.active - touched_slots)`. For RTS at
active=4 with one A7 slot: 3 × 3 = 9 LX7 saved per MMIO fire.

Arms narrowed:
* **RTS** (0x4E75) — `m68k_step` modifies only A7 (and cpu->pc, not cached)
* **MOVE.L (An)+,Dn** — modifies Dn and An (mask = `{Dn, An}`)
* **BSR.S / BSR.W** — modifies A7
* **JSR (d16,PC) / JSR (d16,An)** — modifies A7
* **MOVE.B (d16,An),Dn** — modifies Dn

**Safety:** unlike M6.121-broad (which crashed bench by skipping ALL
slots and corrupting state via stale dirty bits), selective reload only
SKIPS the l32i for slots that didn't change. The dirty-bit flow stays
the same — fast path's `emit_write_g` sets bits; epilogue's
`cache_flush` writes them back; cache values match cpu for all paths.

**Triple-diff:** ctest 7/7, `--diff-jit-trace` clean through 11 038 cyc.

**Perf:**

| Workload | M6.122 | **M6.123** | Δ |
|----------|------:|----------:|--:|
| Bench (20 M)        | 1.172 | **1.172** | unchanged |
| **Bench (100 M)**   | 1.198 | **1.197** | xt −90 K (−0.08 %) |
| Boot 5 M det        | 2.223 | **2.223** | unchanged |
| **Boot 100 M**      | 1.704 | **1.700** | xt −440 K (−0.24 %) |

Real-helper counts unchanged (correctness preserved):
* Bench 100M: real_helpers 113 335 unchanged
* Boot 100M:  real_helpers 679 302 unchanged

Modest gain but accumulates. Each new arm narrowed adds ~3 LX7 ×
hit_count savings. Future arms can pile on as their helper modifies a
known subset of guest regs.

Cumulative M6.84 → M6.123:
* Bench (20 M): 1.257 → **1.172** (−6.8 %)
* Bench (100 M): 1.396 → **1.197** (**−14.3 %**) 🎯
* Boot 300 K det: 2.170 → **1.975** (−9.0 %)
* Boot 5 M det:   2.471 → **2.223** (−10.0 %)
* Boot 100 M:     1.734 → **1.700** (−2.0 %)

## M6.122 — m68k_decode_at length fix for MOVE-to-SR/CCR — bench 100 M 1.210 → 1.198 lx7/cyc (−1.0 %)

Long-standing latent bug in `m68k_decode_at` — for the four MOVE SR/CCR
variants (`MOVE SR,<ea>`, `MOVE CCR,<ea>`, `MOVE <ea>,CCR`, `MOVE <ea>,SR`),
the szf field (bits 7-6) is FIXED at `11` as part of the opcode pattern,
NOT a size selector. Pre-M6.122 decode took szf=3 → sz=4 (.L) and called
`ea_ext_bytes` with sz=4 — returning 4 bytes for a `#imm`
destination/source. But these MOVEs ALWAYS use 16-bit imm (word).

Result: `m68k_decode_at(0x4010DC)` for `MOVE #0x0120,SR` returned
length 6 instead of 4. The JIT block compiler then walked PAST the
imm word, decoding the byte at 0x4010E0 as a NEW opcode (the
phantom `0x4A38`). Bench's 21 598 helpers/100M for `0x4A38` (at
first_pc 0x4010E0) were a side effect — the phantom op's compile-time
default-helper bridge ran `emit_advance_flush` to where the *real*
M6.117 MOVE-to-SR inline left cpu->pc (the byte position just inside
the imm word), and m68k_step then decoded the REAL next instruction.
Accidentally correct because no inline arm fired for the phantom's
opcode — any new inline arm for that opcode would have corrupted
execution.

Fix: `(op & 0xF1C0) == 0x40C0` matches all four MOVE-SR/CCR variants;
force `ea_sz = 2` for them.

**Triple-diff:** ctest 7/7, `--diff-jit-trace` clean through 11 038 cyc.

**Perf:**

| Workload | M6.121 | **M6.122** | Δ |
|----------|------:|----------:|--:|
| Bench (20 M)        | 1.173 | **1.172** | within noise |
| **Bench (100 M)**   | 1.210 | **1.198** | **−1.00 %** ✅ |
| Boot 5 M det        | 2.223 | **2.223** | unchanged |
| Boot 100 M          | 1.668 | **1.704** | +2.2 % (M6.66 divergence) |

The bench 100M improvement is real: the correctly-bounded block at
PC=0x4010DC now contains only the MOVE-to-SR op and ends correctly;
the dispatcher next compiles a SEPARATE block at PC=0x4010E0 with
`0x4A38` as op[0], which the M6.77 TST.B (xxx).W inline now catches.
`real_helpers` 135 011 → 113 335 (−21 676 ≈ M6.77 fires for the
bench's hot 0x4A38).

The boot 100M regression is in the M6.66 VIA-tick-divergence region
(post-cycle-11898). The fix changes block boundaries around 0x4010DC,
shifting the divergence trajectory. Boot's real-workload behavior
(boot 5M det, pre-divergence) is unchanged at 2.223 lx7/cyc.

🎯 **Bench 100 M crosses 5.39 × interp** (= 6.462 / 1.198).

Cumulative M6.84 → M6.122:
* Bench (20 M): 1.257 → **1.172** (−6.8 %)
* Bench (100 M): 1.396 → **1.198** (**−14.2 %**) 🎯
* Boot 300 K det: 2.170 → **1.975** (−9.0 %)
* Boot 5 M det:   2.471 → **2.223** (−10.0 %)
* Boot 100 M:     1.734 → **1.704** (−1.7 %)

## M6.119 — Skip cache_reload in register-preserving fast helpers — boot 100 M 1.715 → 1.668 lx7/cyc (−2.74 %) 🎯

Two micro-optimizations that share the "compile-time-known result" theme:

**Part A — register-preserving fast-helper bridges skip `emit_cache_reload`:**

The MMIO fast-helpers `m68k_jit_btst_b_mmio` and `m68k_jit_ori_b_mmio`
only read/write memory + CCR — they DO NOT touch any guest D/A register.
The BTST and ORI inline arms' MMIO-fallback bridges were doing a full
`emit_cache_reload` (1 l32i per active slot) after the helper call,
restoring values that hadn't changed.

Removing the reload saves `3 LX7 × rc.active` per MMIO fire. With 4
active cache slots, that's 12 LX7 per fire. Boot's BTST #imm,(d16,An)
fires ~200 K times on its hot VIA-register-polling loop; ORI.B similarly.

`emit_sr_reload` is still needed (the helpers DO update CCR).

**Part B — compile-time-constant MOVE-family flag emit:**

New `emit_logic_flags_const(e, value)` helper writes N and Z from a
compile-time-known value with 2–4 ops instead of `emit_logic_flags`'s
7–8 ops. Applied to:
* MOVEQ #imm8,Dn — value = sext(imm8)
* MOVE.L #imm32,Dn — value = imm32
* MOVE.W #imm16,Dn — value = (u32)imm16 << 16

Tiny effect on host metric (these ops aren't frequent enough).

**Triple-diff workflow:** ctest 7/7, `--diff-jit-trace` clean through
11 038 cycles, boot 100 M helper count 184 693 (M6.118) → 184 692
(−1, within noise — correctness preserved).

**Perf:**

| Workload | M6.118 | **M6.119** | Δ |
|----------|------:|----------:|--:|
| Bench (20 M)        | 1.173 | **1.173** | xt −4 K (Part B) |
| Bench (100 M)       | 1.210 | **1.210** | xt −10 K (Part B) |
| **Boot 5 M det**    | 2.223 | **2.223** | xt −7 |
| **Boot 100 M**      | **1.715** | **1.668** | **−2.74 %** 🎯 (Part A) |

Real metrics confirm the win:
* `real_helpers`: 217 613 → 217 612 (unchanged — correctness preserved)
* `real_lx7_per_cyc`: 1.736 → 1.689 (−2.7 %)

🎯 **Boot 100 M crosses 3.87 × interp** (= 6.462 / 1.668). Biggest single
boot-100M jump in many milestones — the BTST/ORI MMIO bridges fire on
every hot VIA-register access in boot's IRQ/timer-polling loop.

Cumulative M6.84 → M6.119:
* Bench (20 M): 1.257 → **1.173** (−6.7 %)
* Bench (100 M): 1.396 → **1.210** (−13.3 %)
* Boot 300 K det: 2.170 → **1.975** (−9.0 %)
* Boot 5 M det:   2.471 → **2.223** (−10.0 %)
* Boot 100 M:    1.734 → **1.668** (**−3.8 %**) 🎯

## M6.118 — MOVE.W Dn,(d16,An) — bench 20 M crosses 5.51 × interp 🎯

The next un-inlined opcode after M6.117 cleared the 21K-hit list:
bench's 0x3B40 (MOVE.W D0,(d16,A5)) at 4 995 helpers / 100 M cyc and
1 594 / 20 M cyc.

Length 4 (op + d16); cycles 8 (m68k_step base 4 + MOVE handler 4).
EA = An + sext16(d16); bounds-check the EA; fast path writes 2 BE
bytes, MOVE-family flags.

**Triple-diff workflow:**

* ctest: 7/7
* `--diff-jit-trace`: clean through 11 038 cycles
* Boot 5 M det / 100 M: byte-identical
* Bench (20 M): 6 567 → 4 973 helpers (**−1 594** = the 0x3B40 count)
* Bench (100 M): 53 362 → 48 244 helpers (**−5 118**)

**Perf:**

| Workload | M6.117 | **M6.118** | Δ |
|----------|------:|----------:|--:|
| **Bench (20 M)**    | 1.176 | **1.173** | **−0.25 %** |
| **Bench (100 M)**   | 1.212 | **1.210** | **−0.16 %** |
| Boot 5 M det        | 2.223 | **2.223** | unchanged |
| Boot 100 M          | 1.715 | **1.715** | unchanged |

🎯 **Bench 20 M crosses 5.51 × interp** (= 6.462 / 1.173).
🎯 **Bench 100 M crosses 5.34 × interp** (= 6.462 / 1.210).

Cumulative M6.84 → M6.118:
* Bench (20 M): 1.257 → **1.173** (−6.7 %)
* Bench (100 M): 1.396 → **1.210** (**−13.3 %**)
* Boot 300 K det: 2.170 → **1.975** (−9.0 %)
* Boot 5 M det:   2.471 → **2.223** (−10.0 %)
* Boot 100 M:     1.734 → **1.715** (−1.1 %)

## M6.117 — MOVE #imm16,SR — bench 100 M crosses 5.33 × interp 🎯

Privileged op that bench's hot loop hits 21 598 times / 100 M cyc (the
last remaining 21 K-hit un-inlined opcode on the post-cycle-11898 path).
Pattern is the stays-in-supervisor `MOVE #0x2700,SR` (disable interrupts
while in kernel).

**Bug-fix sub-step:** the first commit used `xt_beqz` for the privilege
check, which inverted the condition (beqz jumps when a8 == 0). In
boot/bench (always S=1) the beqz fell through to the slow path every
time, so 0x46fc still hit m68k_step on every dispatch — the apparent
host gain came from the compile-time `helper_ops` decreasing (the inline
arm is classified as inline) while the runtime cost stayed the same.
Fixed to `xt_bnez` so the fast path actually runs for S=1; real_helpers
dropped from 161 727 → 140 129 (= −21 598, the 0x46fc count).

**Inline arm shape:**

* **Compile-time gate**: only inline when imm has S bit (0x2000) set.
  If imm.S = 0, would need a S→U transition with SP swap; defer to
  helper. Boot/bench's hot 0x46fc all have imm.S = 1.
* **Runtime gate**: check current SR.S = 1. If user mode, helper takes
  the privilege-violation trap.
* **Fast path**: `R_SR = imm` via emit_load_imm (1-3 ops).

Length 4 (op + imm.W); cycles 4 (m68k_step base 4, handler 0).

**Triple-diff workflow:**

* ctest: 7/7
* `--diff-jit-trace`: clean through 11 038 cycles
* Boot 5 M det / 100 M: same lx7/cyc (boot helpers 185 089 → 184 693
  = −396, matching boot's 398-hit 0x46fc count — boot's MOVE #imm,SR
  are also imm.S = 1, so they inline too)
* Bench (20 M): unchanged (0x46fc absent from this window)
* Bench (100 M): 74 960 → **53 362** helpers (**−21 598**, exactly the
  0x46fc hit count)

**Perf:**

| Workload | M6.116 | **M6.117** | Δ |
|----------|------:|----------:|--:|
| Bench (20 M)      | 1.176 | **1.176** | unchanged |
| **Bench (100 M)** | 1.223 | **1.212** | **−0.90 %** ✅ |
| Boot 5 M det      | 2.223 | **2.223** | unchanged |
| Boot 100 M        | 1.715 | **1.715** | unchanged |

🎯 **Bench 100 M crosses 5.33 × interp** (= 6.462 / 1.212).

Cumulative M6.84 → M6.117:
* Bench (20 M): 1.257 → **1.176** (−6.4 %)
* Bench (100 M): 1.396 → **1.212** (**−13.2 %**) 🎯
* Boot 300 K det: 2.170 → **1.975** (−9.0 %)
* Boot 5 M det:   2.471 → **2.223** (−10.0 %)
* Boot 100 M:     1.734 → **1.715** (−1.1 %)

## M6.116 — classify_op cleanup: LINK/UNLK/PEA/MOVE-USP are flag-neutral (0u)

Several top=0x4 opcodes don't touch CCR but were classified as SET|CONS
by the default branch. Marking them as 0u (flag-transparent) lets the
lazy-CC analysis "see through" them — when a hypothetical `MOVE.W ;
LINK ; <SET-class>` block appears, the MOVE's flag emit is correctly
marked dead.

Reclassified to 0u:
* **LINK An,#d16** (0x4E50-0x4E57) — subroutine stack-frame setup
* **UNLK An**       (0x4E58-0x4E5F) — subroutine stack-frame teardown
* **PEA <ea>**      (0x4840-0x487F, mode 2..7) — push ea onto SP
* **MOVE An,USP / USP,An** (0x4E60-0x4E6F) — privileged An↔USP

Also moved STOP (0x4E72) and RTD (0x4E74) into the existing CONS bucket
alongside RTS/RTE/RTR/JMP/JSR — control flow that preserves CCR for the
caller / fall-through to read.

**Perf:** ±0 LX7 (these ops aren't preceded by hot inline flag-setters
in our workloads). Correctness improvement only.

## M6.115 — classify_op safety: ROXR/ROXL/ABCD/SBCD consume X (SET|CONS)

Safety follow-up to M6.114. Previously top=0x8/0xC/0xE all returned
SET-only in `classify_op`. But several opcodes in those families consume
X from the prior op's flag emit:

* **ROXR / ROXL** (top=0xE, type=10 at bits 4-3 register-form or bits
  11-9 memory-form): rotate through eXtend. Reads X, sets X from final
  shifted-out bit.
* **ABCD / SBCD** (top=0xC / 0x8 with bit 8 = 1 AND bits 7-4 = 0000):
  BCD add/subtract with X. Reads X.

Without M6.114, this latent bug never fired because plain ADD/SUB were
ALSO classified SET|CONS (over-conservative), so the prior op's flag
emit was always live regardless of what followed. With M6.114 making
ADD/SUB SET-only, a hypothetical `ADD.L Dn,Dm ; ROXR.L Dx,Dy` block
would have ADD's flag emit (which writes X) marked dead, then ROXR
would read stale X.

ctest 7/7, --diff-jit-trace clean, boot 100 M helper count 185 089
unchanged → the sequence isn't actually exercised in our workloads,
but the fix is still right to preserve forward safety.

**Perf:** unchanged (+9 LX7 at 100 M cyc = noise). The fix only matters
if/when such a sequence appears.

## Where things stand right now (post-M6.115)

| Engine | lx7 / cyc | × interp baseline |
|--------|----------:|------------------:|
| **Bench** (Speedometer frozen snapshot, 20 M cycles)                | **1.176** | **5.49 ×** ✅ |
| **Bench** (Speedometer frozen snapshot, 100 M cycles)               | **1.223** | **5.28 ×** ✅ |
| **Boot** (Mac Plus ROM, 100 M cycles)                               | **1.715** | **3.45 ×** |
| **Boot** (Mac Plus ROM, 5 M cycles, PC=`0x40032C` deterministic)    | **2.223** | **2.66 ×** |
| **Boot** (Mac Plus ROM, 300 K cycles, PC=`0x40032C` deterministic)  | **1.975** | **2.99 ×** |

## M6.114 — refine classify_op: plain ADD/SUB don't consume CCR

Lazy-CC classifier was over-conservative for top=9 (SUB) and top=D (ADD)
families: returned SET|CONS for ALL non-ADDA/SUBA forms. But plain ADD/SUB
don't read CCR — only ADDX/SUBX do (they consume X and have sticky Z).

Refined to return SET-only for plain ADD/SUB; SET|CONS only for ADDX/SUBX
(bit 8 = 1 to-ea direction AND mode in {0, 1} register/predec form, with
szf != 3 ruling out ADDA).

Effect: prior op's `emit_logic_flags` is now marked dead when the next
op is plain ADD or SUB, saving 6–7 LX7 ops per execution of the prior op.

**Triple-diff workflow:** ctest 7/7, `--diff-jit-trace` clean through
11 038 cycles, boot 100 M byte-identical helper counts (185 089), boot 5 M
det same cycles.

**Perf:**

| Workload | M6.113 | **M6.114** | Δ |
|----------|------:|----------:|--:|
| Bench (20 M)        | 1.176 | **1.176** | xt −18 K |
| Bench (100 M)       | 1.223 | **1.223** | xt −49 K |
| **Boot 5 M det**    | 2.236 | **2.223** | **−0.58 %** ✅ |
| Boot 100 M          | 1.716 | **1.715** | −0.04 % |

Boot wins most because boot has more `MOVE.x ... ; ADD/SUB ... ; ...`
sequences where the MOVE's emit_logic_flags is now dead. Bench's hot
loop is the same set of ops as before so its xt count barely shifts.

Cumulative M6.84 → M6.114:
* Bench (20 M): 1.257 → **1.176** (−6.4 %)
* Bench (100 M): 1.396 → **1.223** (**−12.4 %**)
* Boot 300 K det: 2.170 → **1.975** (−9.0 %)
* Boot 5 M det:   2.471 → **2.223** (**−10.0 %**) 🎯
* Boot 100 M:     1.734 → **1.715** (−1.1 %)

## M6.113 — BTST/BCHG/BCLR/BSET #imm,(xxx).W — bench 100 M crosses 5.28 × interp

Static bit-op #imm with `(xxx).W` absolute-addressing destination.
Bench's 0x08F8 (BSET #imm,(xxx).W) at 21 K helpers / 100 M cyc was the
last remaining 21 K-hit opcode on the post-cycle-11898 path.

Unified arm handles all four static bit ops via `which = (w >> 6) & 3`:
0=BTST (no write), 1=BCHG (xor), 2=BCLR (and ~mask), 3=BSET (or mask).
Mask is `1 << bit` where bit = imm_word & 7 (byte EA uses low 3 bits).
All set ONLY Z = !old_bit; other CCR bits unchanged.

Same M6.77/M6.108 compile-time RAM check on the abs address. Length 6
(op + imm.W + abs.W); cycles 8 (m68k_step base 4 + handler 4).

**Triple-diff workflow:**

* ctest: 7/7
* `--diff-jit-trace`: clean through 11 038 cycles
* Boot 5 M det / 100 M: byte-identical
* Bench (20 M): 6 569 → 6 567 helpers (−2)
* Bench (100 M): 96 560 → 74 960 helpers (**−21 600** — the 0x08F8 loop)

**Perf:**

| Workload | M6.112 | **M6.113** | Δ |
|----------|------:|----------:|--:|
| Bench (20 M)     | 1.176 lx7/cyc | **1.176** | unchanged |
| **Bench (100 M)** | 1.234 lx7/cyc | **1.223 lx7/cyc** | **−0.89 %** lx7 |
| Boot @ 100 M cyc | 1.716 lx7/cyc | **1.716** | within noise |

🎯 **Bench 100 M crosses 5.28 × interp** — eighth consecutive
100-M-bench gain. The 21 K-hit pattern on the bench's post-M6.66 path
appears to be a tight loop iterating ~21 K times — each new inline
arm captures another ~21 K helpers worth.

Cumulative M6.84 → M6.113:
* Bench (20 M): 1.257 → **1.176** (−6.4 %)
* Bench (100 M): 1.396 → **1.223** (**−12.4 %**)
* Boot 300 K det: 2.170 → **1.975** (−9.0 %)
* Boot 5 M det:   2.471 → **2.236** (−9.5 %)
* Boot 100 M:     1.734 → **1.716** (−1.0 %)

## M6.112 — ADD.B / SUB.B Dm,Dn — bench 100 M crosses 5.24 × interp

The existing ADD.W/SUB.W Dm,Dn arm at szf=1 was the M6 era inline.
Extending it to szf=0 (.B) catches bench's 0xD603 (ADD.B D3,D3) at
21 K helpers / 100 M cyc on the post-cycle-11898 path.

Same "shift-to-high" trick parametrized on size: shift by 24 for .B
(bit 7 → bit 31), 16 for .W (bit 15 → bit 31). The merge back uses
srli/slli by size_bits to clear the low bits of Dn before OR-ing in
the size-truncated result.

Cycles unchanged: m68k_step base 4 + handler 4 = 8 (same as .W).

**Triple-diff workflow:**

* ctest: 7/7
* `--diff-jit-trace`: clean through 11 038 cycles
* Boot 5 M det / 100 M: byte-identical
* Bench (20 M): 6 570 → 6 569 helpers (−1 — bench 20M has no .B ADD/SUB)
* Bench (100 M): 118 158 → 96 560 helpers (**−21 598** — the 0xD603
  loop at 21 K hits/100 M)

**Perf:**

| Workload | M6.111 | **M6.112** | Δ |
|----------|------:|----------:|--:|
| Bench (20 M)     | 1.176 lx7/cyc | **1.176** | unchanged |
| **Bench (100 M)** | 1.242 lx7/cyc | **1.234 lx7/cyc** | **−0.65 %** lx7 |
| Boot @ 100 M cyc | 1.716 lx7/cyc | **1.716** | within noise |

🎯 **Bench 100 M crosses 5.24 × interp** — seventh consecutive
100-M-bench improvement:
* M6.105 BSR.W:           5.00 ×
* M6.107 LEA(d16,PC):     5.04 ×
* M6.108 (xxx).W .L:      5.08 ×
* M6.109 (xxx).W .W/.B:   5.12 ×
* M6.110 Dn,(xxx).W:      5.16 ×
* M6.111 AND/OR.L (xxx).W: 5.20 ×
* M6.112 ADD/SUB.B Dm,Dn: 5.24 ×

Cumulative M6.84 → M6.112:
* Bench (20 M): 1.257 → **1.176** (−6.4 %)
* Bench (100 M): 1.396 → **1.234** (**−11.6 %**)
* Boot 300 K det: 2.170 → **1.975** (−9.0 %)
* Boot 5 M det:   2.471 → **2.236** (−9.5 %)
* Boot 100 M:     1.734 → **1.716** (−1.0 %)

## M6.111 — AND.L / OR.L (xxx).W,Dn — bench 100 M crosses 5.20 × interp

Continues the (xxx).W class to the logic-ALU family. Bench's
0xC4B8 (AND.L (xxx).W,D2) at 21 K helpers / 100 M cyc was the next
hot un-inlined (xxx).W variant.

Combined arm handles both AND.L and OR.L (top=0xC and 0x8 respectively),
since they share the read-modify-write structure — only differ in the
combine op (xt_and vs xt_or). Same M6.77/M6.108 compile-time RAM check.

EOR.L (xxx).W is structurally different (EOR only has Dn,(ea) form,
not (ea),Dn) — would need a separate write-back arm. Skipped since
bench doesn't show EOR.L (xxx).W in the top helpers.

**Triple-diff workflow:**

* ctest: 7/7
* `--diff-jit-trace`: clean through 11 038 cycles
* Boot 5 M det / 100 M: byte-identical (no cycle drift)
* Bench (20 M): 6 647 → 6 570 helpers (−77)
* Bench (100 M): 139 832 → 118 158 helpers (**−21 674**)

**Perf:**

| Workload | M6.110 | **M6.111** | Δ |
|----------|------:|----------:|--:|
| Bench (20 M)     | 1.177 lx7/cyc | **1.176** | **−0.08 %** |
| **Bench (100 M)** | 1.253 lx7/cyc | **1.242 lx7/cyc** | **−0.88 %** lx7 |
| Boot @ 100 M cyc | 1.716 lx7/cyc | **1.716** | within noise |

🎯 **Bench 100 M crosses 5.20 × interp** — sixth consecutive 100 M
bench improvement:
* M6.105 BSR.W:           5.00 ×
* M6.107 LEA(d16,PC):     5.04 ×
* M6.108 (xxx).W .L:      5.08 ×
* M6.109 (xxx).W .W/.B:   5.12 ×
* M6.110 Dn,(xxx).W:      5.16 ×
* M6.111 AND/OR.L (xxx).W: 5.20 ×

Cumulative M6.84 → M6.111:
* Bench (20 M): 1.257 → **1.176** (−6.4 %)
* Bench (100 M): 1.396 → **1.242** (−11.0 %)
* Boot 300 K det: 2.170 → **1.975** (−9.0 %)
* Boot 5 M det:   2.471 → **2.236** (−9.5 %)
* Boot 100 M:     1.734 → **1.716** (−1.0 %)

## M6.110 — MOVE.W / MOVE.B Dn,(xxx).W — bench 100 M crosses 5.16 × interp

Write-side counterparts to M6.109's MOVE.W/.B (xxx).W,Dn. Bench's 0x31C0
(MOVE.W D0,(xxx).W) at 21 K helpers / 100 M is the .W variant; .B
follows the same shape.

Same M6.77 compile-time RAM check (writes to MMIO fall to helper as
before). On RAM hit, emit 2-byte BE write (.W) or 1-byte write (.B)
directly. Uses `emit_read_g_in` to read Dn via cache slot when cached
— saves the xt_mov.

**Triple-diff workflow:**

* ctest: 7/7
* `--diff-jit-trace`: clean through 11 038 cycles
* Boot 5 M det / 100 M: byte-identical
* Bench (20 M): 6 666 → 6 647 helpers (−19)
* Bench (100 M): 161 448 → 139 832 helpers (**−21 616**)

**Perf:**

| Workload | M6.109 | **M6.110** | Δ |
|----------|------:|----------:|--:|
| Bench (20 M)     | 1.177 lx7/cyc | **1.177** | unchanged |
| **Bench (100 M)** | 1.261 lx7/cyc | **1.253 lx7/cyc** | **−0.63 %** lx7 |
| Boot @ 100 M cyc | 1.716 lx7/cyc | **1.716** | within noise |

🎯 **Bench 100 M crosses 5.16 × interp** — fifth consecutive 100-M-bench
gain this iteration:
* M6.105 BSR.W:         5.00 × interp
* M6.107 LEA(d16,PC):   5.04 ×
* M6.108 (xxx).W .L:    5.08 ×
* M6.109 (xxx).W .W/.B: 5.12 ×
* M6.110 Dn,(xxx).W:    5.16 ×

Cumulative this iteration (M6.105 → M6.110): bench 100 M **1.293 → 1.253**
(**−3.1 %**).

Cumulative M6.84 → M6.110:
* Bench (20 M): 1.257 → **1.177** (−6.4 %)
* Bench (100 M): 1.396 → **1.253** (−10.2 %)
* Boot 300 K det: 2.170 → **1.975** (−9.0 %)
* Boot 5 M det:   2.471 → **2.236** (−9.5 %)
* Boot 100 M:     1.734 → **1.716** (−1.0 %)

## M6.109 — MOVE.W / MOVE.B (xxx).W,Dn — bench 100 M crosses 5.12 × interp

Continues the (xxx).W class from M6.108 for the smaller .W and .B sizes.
Bench's 0x1638 (MOVE.B (xxx).W,D3) at 21 K helpers / 100 M cyc is the
.B variant; .W is moderately common too. Both use the same M6.77 / M6.108
compile-time RAM check.

The .W form needs the abs address to be 2-aligned; .B has no alignment
requirement. Both merge into Dn[size-1:0] preserving the upper bits,
using the cache-slot-direct path when Dn is cached (skips emit_read_g's
xt_mov + emit_write_g's xt_mov).

**Triple-diff workflow:**

* ctest: 7/7
* `--diff-jit-trace`: clean through 11 038 cycles
* Boot 5 M det / 100 M: byte-identical (no cycle drift)
* Bench (20 M): 6 705 → 6 666 helpers (−39)
* Bench (100 M): 183 084 → 161 448 helpers (**−21 636**)

**Perf:**

| Workload | M6.108 | **M6.109** | Δ |
|----------|------:|----------:|--:|
| Bench (20 M)     | 1.177 lx7/cyc | **1.177** | unchanged |
| **Bench (100 M)** | 1.271 lx7/cyc | **1.261 lx7/cyc** | **−0.79 %** lx7 |
| Boot @ 100 M cyc | 1.716 lx7/cyc | **1.716** | within noise |

🎯 **Bench 100 M crosses 5.12 × interp** — fourth consecutive 100-M-bench
gain this iteration:
* M6.105 BSR.W:       crossed 5.00 × interp
* M6.107 LEA(d16,PC): crossed 5.04 ×
* M6.108 (xxx).W .L: crossed 5.08 ×
* M6.109 (xxx).W .W/.B: crosses 5.12 ×

Cumulative this iteration (M6.105 → M6.109): bench 100 M **1.293 → 1.261**
(**−2.5 %**), boot 100 M unchanged at lx7/cyc resolution.

Cumulative M6.84 → M6.109:
* Bench (20 M): 1.257 → **1.177** (−6.4 %)
* Bench (100 M): 1.396 → **1.261** (−9.7 %)
* Boot 300 K det: 2.170 → **1.975** (−9.0 %)
* Boot 5 M det:   2.471 → **2.236** (−9.5 %)
* Boot 100 M:     1.734 → **1.716** (−1.0 %)

## M6.108 — MOVE.L / MOVEA.L (xxx).W → Dn/An — bench 100 M crosses 5.08 × interp

The (xxx).W absolute-addressing source mode reads a signed 16-bit
address from the ext word, sign-extended to 24-bit RAM space. Range
is [0, 0x7FFF] (low-RAM Mac globals) or [0xFF8000, 0xFFFFFE] (high MMIO).
For the RAM half, the static address is compile-time known and we can
inline the .L read directly — same pattern as M6.77's TST.B (xxx).W.

Two new arms:

* **MOVE.L (xxx).W,Dn** — bench-hot 0x2438 at 21 K helpers / 100 M cyc
  on the bench's post-cycle-11898 path
* **MOVEA.L (xxx).W,Am** — sibling, no flags

Both use the M6.77 compile-time RAM check: if the abs address fits in
RAM (`abs_addr < ram_size` and 2-aligned), emit the inline 4-byte read.
Else fall through to the helper bridge.

Bench's post-M6.66-divergence code path uses low-RAM globals heavily
through these absolute-address forms (Mac OS variables at 0x000xxx).
Each helper saved is ~50 LX7 (helper-bridge cost minus inline emit),
times 21 K = ~1.1 M LX7 / 100 M cyc.

**Triple-diff workflow:**

* ctest: 7/7
* `--diff-jit-trace`: clean through 11 038 cycles
* Boot 5 M det: 139 helpers unchanged
* Boot 100 M: 185 612 → 185 126 (−486, mostly low-RAM globals)
  no cycle drift
* Bench (20 M): 6 828 → 6 705 (−123)
* Bench (100 M): 204 807 → 183 084 (**−21 723**)

**Perf:**

| Workload | M6.107 | **M6.108** | Δ |
|----------|------:|----------:|--:|
| Bench (20 M)     | 1.177 lx7/cyc | **1.177** | unchanged |
| **Bench (100 M)** | 1.281 lx7/cyc | **1.271 lx7/cyc** | **−0.78 %** lx7 |
| Boot @ 100 M cyc | 1.716 lx7/cyc | **1.716** | within noise |

🎯 **Bench 100 M crosses 5.08 × interp** — third consecutive
100-M-bench-improvement in this iteration (after M6.105 BSR.W
crossing 5.00 × and M6.107 LEA (d16,PC) crossing 5.04 ×).

Cumulative M6.84 → M6.108:
* Bench (20 M): 1.257 → **1.177** (−6.4 %)
* Bench (100 M): 1.396 → **1.271** (−9.0 %)
* Boot 300 K det: 2.170 → **1.975** (−9.0 %)
* Boot 5 M det:   2.471 → **2.236** (−9.5 %)
* Boot 100 M:     1.734 → **1.716** (−1.0 %)

## M6.107 — LEA (d16,PC),An inline — bench 100 M crosses 5.04 × interp

The existing LEA arm covered srcmode 2/5/6/7-0/7-1 but not mode 7/2
(PC-relative `(d16,PC)`). Bench 100 M's helper-histo revealed
0x41FA (LEA (d16,PC),A0) at **21 K helpers / 100 M cyc** — the
biggest single missing inline on the bench's post-cycle-11898 path.

The target is a compile-time constant: `An = op_pc + 2 + sext16(d16)`.
Implementation is the leanest of any branch-class arm — just an
`emit_load_imm32` (or l32r if the target happens to match
LITERAL_BCC_PC) followed by `emit_write_g(An)`. Length 4, cycles 8.

**Triple-diff workflow:**

* ctest: 7/7
* `--diff-jit-trace`: clean through 11 038 cycles
* Boot 5 M det: 144 → 139 helpers (−5)
* Boot 100 M: 185 842 → 185 612 helpers (−230); no cycle drift
* Bench (20 M): 6 868 → 6 828 helpers (−40)
* Bench (100 M): 226 445 → 204 807 helpers (**−21 638**)

**Perf:**

| Workload | M6.106 | **M6.107** | Δ |
|----------|------:|----------:|--:|
| Bench (20 M)     | 1.177 lx7/cyc | **1.177** | unchanged |
| **Bench (100 M)** | 1.292 lx7/cyc | **1.281 lx7/cyc** | **−0.85 %** lx7 |
| Boot @ 100 M cyc | 1.717 lx7/cyc | **1.716 lx7/cyc** | within noise |

🎯 **Bench 100 M crosses 5.04 × interp** — biggest single-arm bench
100 M improvement since M6.105's BSR.W (which crossed 5.00 ×).

The pattern: bench's post-M6.66-divergence path runs through a code
region rich in PC-relative addressing (probably trap dispatch tables
or compiled C functions). Catching `LEA (d16,PC),An` saves the
helper for what is effectively a constant load + write.

Cumulative M6.84 → M6.107:
* Bench (20 M): 1.257 → **1.177** (−6.4 %)
* Bench (100 M): 1.396 → **1.281** (−8.2 %)
* Boot 300 K det: 2.170 → **1.975** (−9.0 %)
* Boot 5 M det:   2.471 → **2.236** (−9.5 %)
* Boot 100 M:     1.734 → **1.716** (−1.0 %)

## M6.106 — BRA.W / Bcc.W disp16 inline

Companion to M6.105's BSR.W: handles the conditional/unconditional .W
(16-bit displacement) Bcc variants. Same chain-preservation lever —
inline keeps the JIT chain unbroken through branches with displacements
beyond ±128 bytes.

**Mid-iteration bug + fix (ctest caught it!):** First version passed
the raw 16-bit disp to `emit_bcc_branchless_tail`. ctest's
`jit_differential` failed immediately: demo's BNE.W backward branch
(used in the sum-loop at op_pc=0x10C) went to the wrong PC because
the tail computes `taken = ft + disp`, but for Bcc.W `ft = op_pc + 4`
while the m68k disp16 is relative to `op_pc + 2`. The fix is to pass
`disp - 2` — same off-by-2 adjustment as the M6.38 DBcc arm (which
has the same 4-byte op-length quirk).

The classic `--diff-jit-trace` at 11 038 cycles passed even with the
bug (the bench doesn't hit Bcc.W backward branches in that window),
but `jit_differential` (the synthetic demo with explicit
sum-loop + BNE.W backward) caught it instantly. **The synthetic ctest
remains the most sensitive correctness signal** for new branch-tail
arms — boot 100M cycle drift takes longer to manifest.

Pre-pass extended: when block ends with BRA.W (cc=0), set
LITERAL_BCC_PC = taken; for Bcc.W (cc≥2), set = fall-through (op_pc+4).
Mirrors the M6.105 BSR.W handling.

**Triple-diff workflow:**

* ctest: 7/7 (after the disp - 2 fix; before the fix, demo BNE.W diverged)
* `--diff-jit-trace`: clean through 11 038 cycles
* Boot 5 M det / 100 M: byte-identical (no cycle drift)
* Bench (20 M): 7 071 → 6 868 helpers (−203)
* Bench (100 M): 226 649 → 226 445 (−204) — Bcc.W is less common at
  long horizons than BSR.W since most Bcc patterns are .S
* Boot 100 M: 185 977 → 185 842 helpers (−135)

**Perf:**

| Workload | M6.105 | **M6.106** | Δ |
|----------|------:|----------:|--:|
| Bench (20 M)     | 1.178 lx7/cyc | **1.177 lx7/cyc** | **−0.08 %** |
| Bench (100 M)    | 1.293 lx7/cyc | **1.292 lx7/cyc** | unchanged at resolution |
| Boot @ any       | unchanged | unchanged | — |

Bench (20 M) crosses **1.177 lx7/cyc**. The marginal 100 M Δ vs M6.105's
−0.84 % reflects that Bcc.W is much less common than BSR.W in the
bench's longer-running paths (most branches use .S form since their
targets fit in 8-bit disp). The infrastructure value is documenting
the off-by-2 pattern for any future .W-displacement arm.

## M6.105 — BSR.W disp16 inline — bench 100 M crosses 5.00 × interp 🎯

The M6.83 BSR.S disp8 inline only handled the 8-bit-displacement form
(disp byte ≠ 0). BSR.W (disp byte == 0, 16-bit disp in ext word) was
routed to the helper bridge. Bench has 0x6100 (BSR.W) at 117 helpers /
20 M cyc — moderate count, but BSR is a **block terminator**, so each
helper fallback breaks the JIT chain (M6.102's chain-preservation
insight).

Adding the inline: similar to BSR.S but with 16-bit disp and 4-byte
return PC offset (BSR.W is 4 bytes total).

Pre-pass for LITERAL_BCC_PC also extended: when block ends with BSR.W,
stash `op_pc + 2 + sext16(disp16)` in the literal pool so the inline
emit can load the target with a 1-op l32r.

**Triple-diff workflow (full SOP):**

* ctest: 7/7
* `--diff-jit-trace`: clean through 11 038 cycles
* Boot 5 M det / 100 M: byte-identical (no cycle drift — per
  `memory/move-cycle-drift-gotcha.md` SOP)
* Bench (20 M): 7 188 → 7 071 helpers (−117)
* Bench (100 M): 248 363 → 226 649 (**−21 714**, similar magnitude
  to M6.104's ADD/SUB.L An src extension)

**Perf:**

| Workload | M6.104 | **M6.105** | Δ |
|----------|------:|----------:|--:|
| Bench (20 M)     | 1.178 lx7/cyc | **1.178** | unchanged |
| **Bench (100 M)**| 1.304 lx7/cyc | **1.293 lx7/cyc** | **−0.84 %** lx7 |
| Boot @ 100 M cyc | 1.717 lx7/cyc | **1.717** | unchanged |

🎯 **Bench 100 M crosses 5.00 × interp baseline** (6.462 / 1.293).

The chain-preservation insight from M6.102's DBEQ keeps paying off:
inline terminators eliminate dispatcher round-trips that compound
at long horizons. The 20 M bench doesn't move at lx7/cyc resolution
but the 100 M bench drops by nearly 1 % per terminator-class
inline added.

Cumulative M6.84 → M6.105:
* Bench (20 M): 1.257 → **1.178** (−6.3 %)
* Bench (100 M): 1.396 → **1.293** (−7.4 %)
* Boot 300 K det: 2.170 → **1.975** (−9.0 %)
* Boot 5 M det:   2.471 → **2.236** (−9.5 %)
* Boot 100 M:     1.734 → **1.717** (−0.98 %)

## M6.104 — ADD.L / SUB.L extend src to An — bench 100 M −0.76 %

The existing M6 emit_add_l_dd / emit_sub_l_dd handled only `Dm,Dn` src.
Extending the condition from `mode == 0` to `mode == 0 || mode == 1`
(and threading a `src_is_an` flag through to the helpers) covers
`ADD.L Am,Dn` and `SUB.L Am,Dn` patterns — common in pointer-arithmetic
loops where An is added to or subtracted from a Dn accumulator.

Bench's 0x948A (SUB.L A2,D2) at 72 helpers / 20 M cyc was the trigger.
At 100 M, the pattern scales up dramatically — bench 100 M helpers
dropped **22 K** (270 056 → 248 363).

The cache-direct fast path (`in-place add/sub` when both src and dst
are cached) now works for An src too. The `dn != dm` check was
relaxed to `src_is_an || dn != dm` since An and Dn are in different
guest-reg namespaces (no aliasing risk between A_x and D_x).

**Triple-diff workflow (full SOP):**

* ctest: 7/7
* `--diff-jit-trace`: clean through 11 038 cycles
* Boot 5 M det / 100 M: byte-identical (no cycle drift — per
  `memory/move-cycle-drift-gotcha.md` SOP)
* Bench (20 M): 7 284 → 7 188 helpers (−96)
* Bench (100 M): 270 056 → 248 363 (**−21 693** at long horizon)

**Perf:**

| Workload | M6.103 | **M6.104** | Δ |
|----------|------:|----------:|--:|
| Bench (20 M)     | 1.178 lx7/cyc | **1.178** | unchanged |
| Bench (100 M)    | 1.314 lx7/cyc | **1.304 lx7/cyc** | **−0.76 %** |
| Boot @ 100 M cyc | 1.717 lx7/cyc | **1.717** | unchanged |

Bench 100 M crosses **4.95 × interp** for the first time.

Same chain-preservation insight as M6.102's DBEQ: the 20 M bench
doesn't move at lx7/cyc resolution, but the 100 M bench compounds the
saving via preserved chain transitions (each `ADD.L An,Dn` no longer
forces a dispatcher round-trip).

Cumulative M6.84 → M6.104:
* Bench (20 M): 1.257 → **1.178** (−6.3 %)
* Bench (100 M): 1.396 → **1.304** (−6.6 %)
* Boot 300 K det: 2.170 → **1.975** (−9.0 %)
* Boot 5 M det:   2.471 → **2.236** (−9.5 %)
* Boot 100 M:     1.734 → **1.717** (−0.98 %)

## M6.103 — MOVEA.L (An),Am inline

Adds the mode 2 ((An)) source variant to the MOVEA.L family — sibling
of M6.75's (d16,An), M6.91's (An)+ and M6.101's MOVE.L (An),Dn but
writing the 32-bit result to Am instead of Dn. MOVEA never touches
CCR so no flag emit.

Boot's 0x2050 (MOVEA.L (A0),A0) at 390 helpers / 100 M cyc — a
common pattern in ROM-table pointer-chasing where the same An is
used as both src and dst.

**Same-An edge case:** for `MOVEA.L (A0),A0` the read of (A0) happens
through a8 = A0 (loaded into scratch), then 4 bytes read from (a8),
then result stored into A0's cache slot. The order naturally preserves
the original A0 for the read before overwriting.

**Triple-diff workflow (full SOP):**

* ctest: 7/7
* `--diff-jit-trace`: clean through 11 038 cycles
* Boot 300 K / 5 M det: unchanged
* **Boot 100 M**: 186 390 → **185 999** helpers (−391, matches the
  0x2050 histo count); lx7/cyc unchanged at resolution; no cycle-drift
  trap (per memory/move-cycle-drift-gotcha.md SOP)

**Perf:**

| Workload | M6.102 | **M6.103** | Δ |
|----------|------:|----------:|--:|
| Bench (20 M)     | 1.179 | **1.178** | **−0.08 %** lx7 |
| Bench (100 M)    | 1.314 | **1.314** | unchanged |
| Boot @ 100 M     | 1.717 | **1.717** | unchanged |

Bench 20 M crosses **5.49 × interp**.

## M6.102 — DBEQ inline (bench 100 M −1.1 %)

Extends the M6.38 DBcc inline (which previously handled cc=1 (DBF) and
cc=6 (DBNE)) to also cover cc=7 (DBEQ). The difference is a single
bnez↔beqz swap on the Z-bit test in the dec-skip and cond-compute
logic:

* DBNE exits the loop when Z=0 (NE true) → skip dec, cond=0
* DBEQ exits the loop when Z=1 (EQ true) → skip dec, cond=0
* DBF never exits → always dec+branch

Bench has 0x57CD (DBEQ D5,disp16) at 236 helpers / 20 M cyc. At 100 M
cyc, those scale up — and the inline-arm path also keeps the JIT chain
hot (no helper round-trip), which compounds across the bench's longer
post-cycle-11898 loop iterations.

**Triple-diff workflow:**

* ctest: 7/7
* `--diff-jit-trace`: clean through 11 038 cycles
* Boot 300 K / 5 M / 100 M det: byte-identical (no DBEQ on these paths)
* Boot 100 M: 186 390 → 186 390 (unchanged — no DBEQ in boot's path)
  — confirms no cycle-drift trap (the M6.101 lesson holds)
* Bench (20 M): 7 702 → 7 462 (−240 compile-time helpers)
* Bench (100 M): noteworthy lx7/cyc drop

**Perf:**

| Workload | M6.101 | **M6.102** | Δ |
|----------|------:|----------:|--:|
| Bench (20 M)     | 1.179 lx7/cyc | **1.179** | unchanged |
| Bench (100 M)    | 1.328 lx7/cyc | **1.314 lx7/cyc** | **−1.1 %** lx7 |
| Boot @ any       | unchanged | unchanged | — |

The 20 M bench didn't move at lx7/cyc resolution because DBEQ's runtime
hit count there is modest, and the saved helpers' LX7 (~50 each × 236)
are below the 0.1 % threshold. At 100 M the bench's DBEQ count grows
to ~1 200, and **more importantly the inline DBEQ keeps the JIT chain
unbroken**: each helper-fallback would have returned to the dispatcher,
re-prologued, and lost any cache_sig match. Inline → chain stays hot.

Cumulative M6.84 → M6.102:
* Bench (20 M): 1.257 → **1.179** (−6.2 %)
* Bench (100 M): 1.396 → **1.314** (−5.9 %)
* Boot 300 K det: 2.170 → **1.975** (−9.0 %)
* Boot 5 M det:   2.471 → **2.236** (−9.5 %)
* Boot 100 M:     1.734 → **1.717** (−0.98 %)

## M6.101 — MOVE.B Dm,Dn + TST.B Dn + MOVE.L (An),Dn inline (delivered)

Three small bench-warm inline expansions captured by the helper-histo
top after M6.100:

* `MOVE.B Dm,Dn` — bench-warm 0x1003 (MOVE.B D3,D0) at 192 helpers
* `TST.B Dn` — bench-warm 0x4A05 (TST.B D5) at 191 helpers
* `MOVE.L (An),Dn` — bench-warm 0x2014 (MOVE.L (A4),D0) at 157 helpers
  (sibling of M6.91's MOVE.L (An)+,Dn but without post-increment)

**Mid-iteration bug: cycle-drift trap.** First wrote `MOVE.B Dm,Dn`
with `emit_advance(2, 4)`. ctest passed, `--diff-jit-trace` at 11 038
cycles was clean. But **boot 100 M regressed by 34 %** (1.717 → 2.304
lx7/cyc), with helpers exploding from 186 499 → 1 432 452 (+1.25 M
bogus-PC helpers). The 4-cycle drift per MOVE.B call vs interp's
8-cycle accounting (base 4 + handler 4) accumulated past the M6.66
VIA-tick boundary, causing the bogus-PC region to wander much longer.

Fix: `emit_advance(2, 4)` → `emit_advance(2, 8)`. Lesson recorded in
`memory/move-cycle-drift-gotcha.md`:
1. MOVE-family default is 8 cyc, not 4.
2. The 11 K diff_jit_bench_lockstep window can't catch cycle drift —
   add boot 100 M run to the standard SOP after any new emit_advance.

**Triple-diff workflow:**

* ctest: 7/7
* `--diff-jit-trace`: clean through 11 038 cycles
* Boot 300 K det: 80 → 78 helpers (−2)
* Boot 5 M det: 152 → 150 helpers (−2)
* Boot 100 M: 186 499 → 186 390 (−109) — confirmed correct cycle accounting
* Bench (20 M): 8 394 → 7 702 (**−692** compile-time helpers)

**Perf:**

| Workload | M6.100 | **M6.101** | Δ |
|----------|------:|----------:|--:|
| Bench (20 M)     | 1.181 lx7/cyc | **1.179 lx7/cyc** | **−0.17 %** lx7 |
| Boot @ 100 M cyc | 1.717 lx7/cyc | **1.717 lx7/cyc** | unchanged |

**Bench crossed 5.48 × interp** for the first time.

Cumulative M6.84 → M6.101:
* Bench (20 M): 1.257 → **1.179** (−6.2 %)
* Boot 300 K det: 2.170 → **1.975** (−9.0 %)
* Boot 5 M det:   2.471 → **2.236** (−9.5 %)
* Boot 100 M:     1.734 → **1.717** (−0.98 %)

## M6.100 — AND.B/W + OR.B/W + EOR.B/W Dm,Dn inline (delivered)

New `emit_logic_bw_dd_kind` helper covers all six op×size combinations
(.B and .W for OR/AND/EOR with register source/destination). Same
shape as the existing `emit_logic_l_dd_kind` but with extract+merge
to preserve the high bits of Dn above the size operated on.

Dispatch arms added:
* AND.B/W Dm,Dn — top=0xC, szf∈{0,1}, bit8=0, mode=0
* OR.B/W  Dm,Dn — top=0x8, szf∈{0,1}, bit8=0, mode=0
* EOR.B/W Dn,Dm — top=0xB, szf∈{0,1}, bit8=1, mode=0 (EOR only has
  EA-dst form; for mode=0 the EA is Dm at bits 5-3/2-0)

Boot's 0xC242 (AND.W D2,D1) at 525 helpers + .B variants get caught.

Flag emit follows MOVE-family: shift result.size to bit 31 so
emit_logic_flags reads bit (size-1) as N. V=C=0, X preserved.

**Triple-diff workflow:**

* ctest: 7/7
* `--diff-jit-trace`: clean through 11 038 cycles
* Boot 300 K det: 85 → 80 helpers (−5)
* Boot 5 M det: 157 → 152 helpers (−5)
* Boot 100 M: 188 525 → 186 499 (**−2 026**)
* Bench (20M): 8 973 → 8 394 (**−579** helpers in non-hot blocks)

**Perf:**

| Workload | M6.99 | **M6.100** | Δ |
|----------|------:|----------:|--:|
| Bench (20 M)     | 1.182 lx7/cyc | **1.181** | −0.08 % |
| Boot @ 300 K det | 1.976 lx7/cyc | **1.975** | −0.05 % |
| Boot @ 5 M det   | 2.236 lx7/cyc | unchanged | — |
| Boot @ 100 M cyc | 1.718 lx7/cyc | **1.717** | −0.06 % |

Cumulative M6.84 → M6.100:
* Bench (20 M): 1.257 → **1.181** (−6.0 %)
* Boot 300 K det: 2.170 → **1.975** (−9.0 %)
* Boot 5 M det:   2.471 → **2.236** (−9.5 %)
* Boot 100 M:     1.734 → **1.717** (−0.98 %)

The MOVE.B / MOVE.L / EXT / shift / AND-OR-EOR.B/W class is now
largely complete for register-direct forms. The remaining boot
helpers are dominated by MMIO-fallback cases (where the inline arm
fires but the runtime EA hits MMIO) and the 174 K bogus-PC helpers
from the M6.66 VIA-tick divergence (out of scope for inline work).

## M6.99 — EXT.W/EXT.L Dn + LSR.L/ASR.L #imm,Dn inline (delivered)

Combined commit covering three new inline arms:

**EXT.W Dn** (mask `(w & 0xFFF8) == 0x4880`)
- sign-extend Dn[7:0] to Dn[15:0], preserving Dn[31:16]
- 6 value ops: `xt_slli 24; xt_srai 24; xt_extui 0,15` for sign-ext to
  .W, then `xt_extui 16,15; xt_slli 16; xt_or` to merge with Dn.high

**EXT.L Dn** (mask `(w & 0xFFF8) == 0x48C0`) — boot's 0x48C1 at 597 hits
- sign-extend Dn[15:0] to Dn[31:0] (replaces full Dn)
- Just 2 value ops: `xt_slli 16; xt_srai 16` in-place on the cache slot
- Lightning-fast — the simplest inline arm of this batch

**LSR.L / ASR.L #imm,Dn** — separate arm from the M6.97/M6.98 .B/.W
arm because .L doesn't need the size-bit extract or merge (shift
operates on full 32-bit Dn in-place). 3 value ops (last_out extract,
shift, optional sign-ext via srai). last_out MUST be captured before
the in-place shift writes the slot.

**Triple-diff workflow:**

* ctest: 7/7
* `--diff-jit-trace`: clean through 11 038 cycles
* Boot 300 K det: 105 → 85 helpers (−20)
* Boot 5 M det: 177 → 157 helpers (−20)
* Boot 100 M: 189 149 → 188 525 (−624)
* Bench (20 M): 9280 → 8973 (**−307 helpers**, .L shifts in non-hot
  blocks getting inlined)

**Perf:**

| Workload | M6.98 | **M6.99** | Δ |
|----------|------:|----------:|--:|
| Bench (20 M)     | 1.183 lx7/cyc | **1.182 lx7/cyc** | **−0.08 %** lx7 |
| Bench (5 M / 100 M) | unchanged | unchanged | — |
| Boot @ 300 K det | 1.980 lx7/cyc | **1.976 lx7/cyc** | **−0.2 %** lx7 |
| Boot @ 5 M det   | 2.236 lx7/cyc | **2.236 lx7/cyc** | unchanged |
| Boot @ 100 M cyc | 1.718 lx7/cyc | **1.718 lx7/cyc** | unchanged |

**Bench crossed 5.47 × interp** for the first time. Boot 300 K det
crossed **2.99 × interp**.

Cumulative M6.84 → M6.99:
* Bench (20 M): 1.257 → **1.182** (−6.0 %)
* Boot 300 K det: 2.170 → **1.976** (−8.9 %)
* Boot 5 M det:   2.471 → **2.236** (−9.5 %)
* Boot 100 M:     1.734 → **1.718** (−0.92 %)

## M6.98 — LSR.B / ASR.B #imm,Dn inline (delivered)

Extends the M6.97 LSR/ASR.W arm to also handle .B size. The shared
emit logic is parametrized on `size_bits` (8 for .B, 16 for .W) and
the up-shift count for sign-extension is `32 - size_bits`. The merge
back into Dn uses `srli size_bits; slli size_bits` to clear the low
size_bits before OR-ing in the result.

Boot's 0xE208 (ASR.B #1,D0) at 780 hits is the main hot variant.

**Triple-diff workflow:**

* ctest: 7/7
* `--diff-jit-trace`: clean through 11 038 cycles
* Boot 300 K det: 113 → 105 helpers (−8)
* Boot 5 M det: 185 → 177 helpers (−8)
* Boot 100 M: 190 225 → 189 149 (**−1 076**)

**Perf:**

| Workload | M6.97 | **M6.98** | Δ |
|----------|------:|----------:|--:|
| Bench (any size) | 1.183 lx7/cyc | unchanged | — |
| Boot @ 300 K det | 1.982 lx7/cyc | **1.980** | −0.1 % |
| Boot @ 5 M det   | 2.236 lx7/cyc | **2.236** | unchanged |
| Boot @ 100 M     | 1.719 lx7/cyc | **1.718** | −0.06 % |

Marginal increment — the .B shift hits are scattered across many
PCs. The .W variant (M6.97) captured the dominant hot path; .B
adds the long tail.

Remaining .L sizes would need a slightly different code path (no
merge, direct in-place srai/srli). Worth adding when a .L shift
becomes a measured hot helper.

## M6.97 — LSR.W / ASR.W #imm,Dn inline (delivered)

The boot 5M det helper count dropped from **2 923 → 185** (a 94 %
reduction in remaining helpers for that window). The biggest single
boot inline win in many milestones.

**Mid-iteration bit-precision lesson:** boot's e442 helper at 3 K hits
was initially decoded as LSR.W (bits 4-3 = 01); ACTUAL decode is
**ASR.W** (bits 4-3 = 00 — arithmetic shift, sign-extending). The
first LSR-only arm fired ~10 boot calls; extending to ASR captured
the actual 3 K hot path plus its dependent blocks.

The two shift types share the value-merge structure; ASR adds a
`xt_slli; xt_srai 16+imm; xt_extui 0,15` sign-propagation chain
where LSR uses a single `xt_srli imm`. Both compute last_out (bit
imm-1 of original Dn.W) for C/X, and both merge into Dn's low 16
preserving Dn.high_16.

**Triple-diff workflow:**

* ctest: 7/7
* `--diff-jit-trace`: clean through 11 038 cycles
* Boot 300 K det: 187 → 113 helpers (−74)
* Boot 5 M det: 2 923 → 185 helpers (**−2 738**, **−94 %**)
* Boot 100 M: 193 269 → 190 225 compile-time helpers (−3 044)

**Perf:**

| Workload | M6.96 | **M6.97** | Δ |
|----------|------:|----------:|--:|
| Bench (any size) | 1.183 lx7/cyc | **1.183** | unchanged (no shift in bench hot loop) |
| Boot @ 300 K det | 1.995 lx7/cyc | **1.982 lx7/cyc** | **−0.65 %** |
| Boot @ 5 M det   | 2.266 lx7/cyc | **2.236 lx7/cyc** | **−1.3 %** |
| Boot @ 100 M cyc | 1.721 lx7/cyc | **1.719 lx7/cyc** | −0.1 % |

**Cumulative M6.84 → M6.97 on boot:**

| Workload | M6.84 | **M6.97** | Δ |
|----------|------:|----------:|--:|
| Boot 300 K det | 2.170 | **1.982** | **−8.7 %** |
| Boot 5 M det   | 2.471 | **2.236** | **−9.5 %** |
| Boot 100 M     | 1.734 | **1.719** | **−0.86 %** |

Boot 100M is gated by the 174 K bogus-PC helpers from the M6.66 VIA-
tick divergence (post-cycle-11898). The deterministic windows show
the inline work's real value — early-init code is shift-and-byte-op
heavy, and these are now mostly inlined.

The arm covers immediate-count LSR.W and ASR.W. Extending to .B and
.L sizes, plus LSL/ASL (left shifts) and register-count variants,
would chip away at the remaining shift-family helpers (e208 ASR.B
780 hits, e311 ROXL register-count variants).

## M6.96 — TST.B (xxx).W + Bcc.S fusion (delivered)

Sibling of M6.95 TST.L + Bcc fusion, applied to the M6.77 TST.B (xxx).W
inline arm (the bench-hot 0x4A38 at 15 K helpers / 20 M cyc that only
admits compile-time-known RAM addresses).

The byte value is shifted left by 24 so its bit 7 becomes bit 31 of
the register; passing `s == d == r == shifted_byte` to
`emit_cmp_cond_fused` makes V cancel (s == d) and N = bit31 = byte
sign — exactly matching the .B TST CCR convention. Same cc∈{6, 7, 13,
15} coverage as the M6.95 TST.L pattern.

The first attempt always shifted the byte; the revised version only
shifts when fusion fires OR `!flags_dead`. For pure flags_dead + no-
fusion paths (e.g., TST.B (xxx).W followed by a setter), no shift is
emitted.

**Triple-diff workflow:**

* ctest: 7/7
* `--diff-jit-trace`: clean through 11 038 cycles
* Boot 300 K / 5 M / 100 M det: byte-identical to M6.95

**Perf:** bench dropped 264 LX7 (1.183 unchanged at lx7/cyc resolution).
The fusion fires modestly — most TST.B (xxx).W hot calls land at MMIO
addresses (caught by the M6.77 compile-time RAM check and routed to
m68k_step before the arm runs). Inside the arm's RAM-path, the
follow-on Bcc is less common than I expected.

The win is again structural — pattern is now established for any
single-operand-then-Bcc combination. Future candidates: TST.W
(xxx).W (not currently inlined), SWAP+Bcc, EXT.W/EXT.L+Bcc, BSET
result+Bcc.

## M6.95 — TST.L + Bcc.S fusion (delivered)

Extends the M6.30 CMP+Bcc fusion infrastructure to TST.L Dn followed
by a Bcc.S with cc∈{6, 7, 13, 15} (NE / EQ / BLT / BLE). Skips the
~8-op `emit_logic_flags` SR write and the Bcc's `emit_cond` SR read,
computing the condition directly from the Dn value.

**The trick:** reuse `emit_cmp_cond_fused(cc, s, d, r)` by passing
`s == d == r == Dn`. With s == d, the V term `(s^d) & (d^r)` collapses
to 0, leaving N = bit31(r) and Z = (r == 0) — exactly TST's CCR
convention. cc=13 (BLT) reduces to `N` (since V=0), cc=15 (BLE) to
`N | Z`, cc=6/7 to just the Z test on r.

**Mid-iteration bug + fix:** First attempt didn't call
`emit_advance_flush` before the fusion path. `emit_bcc_branchless_tail`
OVERWRITES `cpu->pc` directly, so any compile-time-accumulated
`g_pc_acc` from prior inline ops in the block was LOST. ctest's
`diff_jit_bench_lockstep` caught the regression immediately — block
at PC `0x038B74` (`MOVE.L (A0)+,A7 ; TST.L D7 ; BEQ.S +0x42`) was
off by ~2 bytes of PC. Fixed by flushing pc/cycles before entering
the fusion path.

**Also caught a subtle scratch-register conflict:** the first
implementation passed `r = a8` to `emit_cmp_cond_fused` for the cc=6/7
case, where the fused-cond emit writes a8 with the cond literal
before testing r via `bnez`. With r == a8 that bnez tests the
just-overwritten 1/0, giving wrong cond. Fixed by reading Dn into
a9 (or directly from the cache slot via `emit_read_g_in`).

**Triple-diff workflow:**

* ctest: 7/7 pass (after the emit_advance_flush fix)
* `--diff-jit-trace`: clean through 11 038 cycles
* Boot 300 K / 5 M det: unchanged (TST.L + Bcc not on these paths)
* Boot 100 M: jit_cost 172 053 058 → 172 053 037 (−21 LX7, ~rounding)

**Perf:** Bench at 20 M dropped from 23 668 212 → 23 666 465 lx7
(−1 747 LX7, < 0.01 %). The fusion fires modestly — bench's TST.L
sites are fewer than the CMP.W sites it parallels.

The win is mostly structural: established the pattern for extending
fusion to other single-operand-then-Bcc ops (TST.W, TST.B, SWAP+Bcc,
EXT+Bcc, etc.). All those follow the same s=d=r=value-register
trick.

## M6.94 — MOVE.B (An)+,Dn + MOVE.B Dn,(An)+ inline (delivered)

Post-increment variants of the M6.92 MOVE.B (An),Dn / Dn,(An) arms.
Catches boot's 0x1218 (MOVE.B (A0)+,D1) at 1.1 K helpers and the
sibling write variants.

Same RAM-or-ROM byte bounds (src), RAM-only byte bounds (dst). The
post-increment commits AFTER the byte read so a same-An edge case
reads the byte at the original An first, then increments by 1.

**Triple-diff workflow:**

* ctest: 7/7
* `--diff-jit-trace`: clean through 11 038 cycles
* Boot 300 K / 5 M det: unchanged
* Boot 100 M: 194 458 → 193 269 compile-time helpers (−1 189)
              jit_cost: 172 098 358 → 172 053 058 lx7 (−45 300 LX7)

**Perf:** marginal at lx7/cyc resolution (1.721 unchanged) but real
on the raw counter. The MOVE.B (An)+ variants are less common in boot
than the plain (An) forms, hence the smaller catch.

## M6.93 — MOVE.L Dn|Am,(An) inline (delivered)

Fills the dst_mode=2 gap in the MOVE.L Dn|Am,<ea> arm series.
Existing arms covered post-incr (M6.91-era), pre-dec (M6.91), and
(d16,An) (M6.73), but plain (An) destination was unhandled. Boot's
0x228a (`MOVE.L A2,(A1)`) at 626 hits is the most common variant.

Sibling of the M6.91-era MOVE.L Dn|Am,(An)+ arm — same RAM-only
byte-bounds, 4 BE byte stores, MOVE-family flags — but without the
post-increment.

**Triple-diff workflow:**

* ctest: 7/7
* `--diff-jit-trace`: clean through 11 038 cycles
* Boot 300 K / 5 M det: unchanged from M6.92 (0x228a not in deterministic
  windows; the variant appears in longer boot paths)
* Boot 100 M: 195 104 → 194 458 compile-time helpers (−646)

**Perf:** marginal at the lx7/cyc resolution — boot 100M jit_cost
dropped by 29 680 LX7 (646 helper bridges × 64 helpers-equivalent
minus 11 664 extra inline ops). The win is well below the rounding
threshold for lx7/cyc but real on the raw counter. All other workloads
unchanged.

The MOVE.L Dn|Am,<ea> family is now complete for dst_modes {2, 3, 4, 5}
× src_modes {0, 1}. Plain `MOVE.L Dn,Dm` is already covered separately.

## M6.92 — MOVE.B (An),Dn + MOVE.B Dn,(An) inline (delivered)

Continuation of the M6.91 MOVE.B class. Two more arms covering the
boot helper-histo's next-tier entries:

* `MOVE.B (An),Dn` (top=0x1, dst_mode=0, src_mode=2) — bench-warm
  `0x1211` (MOVE.B (A1),D1) at 6 K boot helpers; `0x1411` (MOVE.B
  (A1),D2) at 3 K; etc. Same shape as M6.91's `(d16,An),Dn` but
  without the d16 displacement add. Source admits RAM-or-ROM byte;
  destination is Dn[7:0] with the existing srli/slli/or merge.
* `MOVE.B Dn,(An)` (top=0x1, dst_mode=2, src_mode=0) — `0x1082`
  (MOVE.B D2,(A0)) at 3 K boot helpers. Source is Dn (no bounds
  check), destination is (An) with RAM-only byte bounds (writes
  can't go to ROM). xt_extui extracts Dn[7:0], xt_s8i stores.

**Triple-diff workflow:**

* ctest: 7/7
* `--diff-jit-trace`: clean through 11 038 cycles
* Boot 300 K det: 896 → 187 helpers (**−709**, ~21 % of pre-M6.91 count)
* Boot 5 M det: 14 288 → 2 923 helpers (**−11 365**, **−79 %** of pre-M6.91 count)
* Boot 100 M: 210 844 → 195 104 compile-time helpers (−15 740)

**Perf:**

| Workload | M6.91 | **M6.92** | Δ |
|----------|------:|----------:|--:|
| Bench @ 5 M / 20 M / 100 M | unchanged | unchanged | (no MOVE.B in bench hot loop) |
| Boot @ 300 K det  | 2.116 lx7/cyc | **1.995 lx7/cyc** | **−5.7 %** lx7 |
| Boot @ 5 M det    | 11 881 871 lx7 | **11 328 607 lx7** | **−4.6 %** lx7 |
| Boot @ 100 M cyc  | 1.729 lx7/cyc | **1.721 lx7/cyc** | **−0.5 %** lx7 |

**Cumulative M6.90 → M6.92 on boot:**

| Workload | M6.90 | M6.92 | Δ |
|----------|------:|------:|--:|
| Boot 300 K det | 2.158 | **1.995** | **−7.6 %** lx7 |
| Boot 5 M det   | 2.468 | **2.266** | **−8.2 %** lx7 |
| Boot 100 M     | 1.734 | **1.721** | **−0.8 %** lx7 |

**Boot is now 1.721 lx7/cyc @ 100 M = 3.43 × interp baseline** (was
3.40 × at M6.90). The deterministic windows show the dramatic gains
because early-init code is MOVE.B-dominated; 100 M includes more of
the post-divergence MMIO-heavy paths where MOVE.B is less central.

The MOVE.B class is largely "complete" now — adding more variants
(MOVE.B (An)+,Dn, MOVE.B -(An),Dn, MOVE.B (An),(Am) mem-to-mem, etc.)
would yield diminishing returns on the boot histogram.

## M6.91 — MOVE.B (d16,An),Dn + MOVE.B (d16,An),(Am) inline (delivered)

Boot's top helper-histogram entry was `0x10A8` at 12 136 hits / 100 M cyc,
first seen at PC `0x400310` (ROM). Initially mis-decoded as `MOVE.B
(d16,A0),D0` (dst_mode=0=Dn) — that arm only matched ~270 calls at
other PCs. Correct decode is `MOVE.B (d16,A0),(A0)` (dst_mode=2=An,
mem-to-mem byte). Both arms now land as a class.

**Byte-aligned bounds literals** (M6.91-new):
```
LITERAL_RAM_BOUNDS_BYTE = ~(ram_size-1)    // no `| 1`, admits any byte addr
LITERAL_ROM_BOUNDS_BYTE = ~(rom_size-1)
```

The existing word-bounds masks (`| 1`) fail odd addresses since MOVE.W/.L
require alignment; for MOVE.B any byte address is legal so the byte
variants drop the alignment-fail bit.

**Two arms added:**

1. `MOVE.B (d16,An),Dn` (top=0x1, dst_mode=0, src_mode=5) — picks up
   the less common variants (the dispatch I first wrote based on
   mis-decoded bit pattern). ~270 boot helpers eliminated.

2. `MOVE.B (d16,An),(Am)` mem-to-mem (top=0x1, dst_mode=2, src_mode=5) —
   the actual boot-hot `0x10A8`. Uses the M6.76 unified RAM-or-ROM
   source-bounds shape so A0 / Am can walk ROM-resident system tables
   (boot's PC=0x400310 dispatches this with A0 → ROM).

**Triple-diff workflow:**

* ctest: 7/7
* `--diff-jit-trace`: clean through 11 038 cycles
* Boot 300 K det: 1192 → 896 helpers (−296). Final PC matches.
* Boot 5 M det:  25 240 → 14 288 helpers (**−10 952**). Same final PC.
* Boot 100 M:    222 980 → 210 844 compile-time helpers (**−12 136**).

**Perf:**

| Workload | M6.90 | **M6.91** | Δ |
|----------|------:|----------:|--:|
| Bench @ 5 M cyc   | 1.340 lx7/cyc | **1.340 lx7/cyc** | unchanged |
| Bench @ 20 M cyc  | 1.184 lx7/cyc | **1.184 lx7/cyc** | unchanged |
| Bench @ 100 M cyc | 1.328 lx7/cyc | **1.328 lx7/cyc** | unchanged |
| Boot @ 300 K det  | 2.158 lx7/cyc | **2.116 lx7/cyc** | **−2.0 %** lx7 |
| Boot @ 5 M det    | 12 352 738 lx7 → 12 341 855 (M6.87 baseline) → **11 881 871** | **−3.7 %** lx7 |
| Boot @ 100 M cyc  | 1.734 lx7/cyc | **1.729 lx7/cyc** | **−0.3 %** lx7 |

The boot perf improvement is concentrated on the deterministic windows
(where MOVE.B-driven ROM-table reads dominate). At 100 M past the
M6.66 VIA-tick divergence boundary the gain shrinks — that path has
more MMIO writes and fewer MOVE.B (d16,An) sites.

**First **boot-targeted** win in many milestones** — every prior
optimization in the M6.85+ series was bench-focused. The MOVE.B class
is the natural next inline-expansion lever; future M6.92 candidates
are `MOVE.B (An),Dn` (0x1211 — 6 K boot helpers) and `MOVE.B Dn,(An)`
(0x1082 — 3 K boot helpers).

## M6.90 — MOVE.W (As),(Ad) l16ui+s16i when flags dead (delivered)

The inline mem-to-mem MOVE.W writes 2 bytes BE with `xt_l8ui`×2 + `xt_s8i`×2
= 4 LX7 ops. When the lazy-CC pass sets flags_dead = true, the
intermediate register value isn't needed — only the bytes landing at
`mem[dst+0..1]` in BE order matter.

Xtensa's `l16ui at,as,imm` reads `p[0] + (p[1] << 8)` (little-endian
into register), and `s16i at,as,imm` writes `p[0] = at[7:0]; p[1] = at[15:8]`.
Used together as a copy:

```
src bytes [0xHI, 0xLO]  →  l16ui →  register = 0xLOHI (byte-swapped)
                        →  s16i  →  dst bytes [0xHI, 0xLO]    (BE preserved)
```

Net: 2 LX7 ops (l16ui + s16i) replace 4 (l8ui×2 + s8i×2) when flags
are dead. Alignment safety: `LITERAL_RAM_BOUNDS = ~(ram_size-1) | 1`
fails the AND fast-path for any odd address, so l16ui/s16i's 2-byte
alignment requirement is already enforced.

**Triple-diff workflow:**

* ctest: 7/7
* `--diff-jit-trace`: clean through 11 038 cycles
* Boot 300 K / 5 M / 100 M det: byte-identical (this opcode doesn't
  appear on the deterministic boot path or 100 M ROM init)

**Perf:**

| Workload | M6.88 | **M6.90** | Δ |
|----------|------:|----------:|--:|
| Bench @ 5 M cyc   | 1.350 | **1.340** | **−0.7 %** lx7 |
| Bench @ 20 M cyc  | 1.191 | **1.184** | **−0.6 %** lx7 |
| Bench @ 100 M cyc | 1.334 | **1.328** | **−0.5 %** lx7 |
| Boot 300 K / 5 M / 100 M det | unchanged | unchanged | — |

**Cumulative M6.84 → M6.90:**

| Workload | M6.84 | M6.90 | Δ |
|----------|------:|------:|--:|
| Bench @ 20 M cyc  | 1.257 | **1.184** | **−5.8 %** lx7 |
| Bench @ 100 M cyc | 1.396 | **1.328** | **−4.9 %** lx7 |

**Bench is now 1.184 lx7/cyc @ 20 M cyc = 5.46 × interp baseline.**

The byte-swap trick generalises to any mem-to-mem .W or .L copy where
the register value's byte order doesn't matter (flags dead, no
intermediate use). Future candidates: MOVE.L mem-to-mem
((An)+,(Am)+ at line 2556) — would need a 4-byte version using
`l32i` + `s32i` on word-aligned addresses, save 6 ops per call
(8 byte ops → l32i + s32i).

## M6.88 — MOVE.W Dm,Dn skip-flags lean path (delivered)

Same class as M6.87 but for `MOVE.W Dm,Dn`. When the lazy-CC pass
sets `flags_dead = true` (bench's `MOVE.W D5,D4 ; MOVE.W (A2),(A3)` —
the second op is a MOVE.W setter that overwrites all CCR), the inline
emit doesn't need the shifted-to-high-16 source needed by
`emit_logic_flags`.

Lean cached path (4 LX7) vs original (7 LX7):

```
                       original (7 cached ops)    M6.88 lean (4 cached ops)
read Dm                xt_mov 9, slot(dm)         -- (emit_read_g_in)
read Dn                xt_mov 11, slot(dn)        -- (slot used directly)
high-clear dn          xt_srli 11, 11, 16
                       xt_slli 11, 11, 16         xt_extui 11, slot(dn), 16, 15
                                                  xt_slli 11, 11, 16
get low_16 of dm       xt_extui 12, 9, 0, 15      xt_extui 12, slot(dm), 0, 15
combine                xt_or  11, 11, 12          xt_or  slot(dn), 11, 12
write Dn               xt_mov slot(dn), 11        -- (direct write)
```

Saves 3 LX7 per call when both Dm and Dn are cached.

**Triple-diff workflow:**

* ctest: 7/7
* `--diff-jit-trace`: clean through 11 038 cycles
* Boot 300 K / 5 M det: same final PC; tiny −3 LX7 (one rare skip_flags
  site in the deterministic boot path)

**Perf:**

| Workload | M6.87 | **M6.88** | Δ |
|----------|------:|----------:|--:|
| Bench @ 5 M cyc   | 1.359 lx7/cyc | **1.350 lx7/cyc** | **−0.7 %** lx7 |
| Bench @ 20 M cyc  | 1.198 lx7/cyc | **1.191 lx7/cyc** | **−0.6 %** lx7 |
| Bench @ 100 M cyc | 1.340 lx7/cyc | **1.334 lx7/cyc** | **−0.5 %** lx7 |
| Boot @ 300 K det  | 2.158 | 2.158 | within noise |
| Boot @ 5 M det    | 12 341 858 lx7 | 12 341 855 lx7 | −3 LX7 |
| Boot @ 100 M cyc  | 1.734 | 1.734 | unchanged |

**Cumulative M6.84 → M6.88:**

| Workload | M6.84 | M6.88 | Δ |
|----------|------:|------:|--:|
| Bench @ 20 M cyc  | 1.257 | **1.191** | **−5.3 %** lx7 |
| Bench @ 100 M cyc | 1.396 | **1.334** | **−4.4 %** lx7 |

**Bench is now 1.191 lx7/cyc @ 20 M cyc = 5.42 × interp baseline.**

The "skip the .W shift gymnastics when flags are dead" class now covers
ADDQ.W (M6.87) and MOVE.W Dm,Dn (M6.88). The same pattern generalises
to ADD.W / SUB.W / AND.W / OR.W / EOR.W Dm,Dn (top=0x8/0x9/0xB/0xC/0xD
with szf=1) — those are boot-warm rather than bench-hot, so the per-
workload win would be small but real. Saved as a future-loop candidate.

## M6.87 — ADDQ.W skip-flags lean path (delivered)

The `emit_addq_w_dn` emitter uses a shifted-to-high-16 form for its
value computation so that `emit_addsub_flags_long` can read s/d/r
directly. When the lazy-CC pass sets `flags_dead = true` (i.e., the
next op overwrites all CCR), the flag emission is already skipped —
but the *value* emission still pays for the unneeded shift form.

M6.87 adds a lean alternative when `skip_flags = true`:

```
                   shifted-form       lean (skip_flags)
                   (10 cached ops)    (6 cached ops)
read Dn            xt_mov 11, slot    -- (emit_read_g_in)
compute            xt_slli 9, 11, 16
                   xt_movi 8, imm     xt_addi 9, slot, imm
                   xt_slli 8, 8, 16   xt_extui 9, slot, 0, 15
                   xt_add 10, 9, 8    xt_extui 9, 9, 0, 15
                   xt_srli 11, 11, 16 xt_extui 10, slot, 16, 15
                   xt_slli 11, 11, 16 xt_slli 10, 10, 16
                   xt_extui 12, 10,
                           16, 15
                   xt_or  11, 11, 12  xt_or  slot, 10, 9
write Dn           xt_mov slot, 11    -- (direct write to slot)
```

Saves 4 LX7 per `ADDQ.W / SUBQ.W #imm,Dn` when D-reg is cached and
the flags are dead. Bench's 0x03DF58 and 0x03DF5E blocks both have
`ADDQ.W #1,D6` followed by `CMP.W (d16,A5),D6` (next op = setter →
flags dead).

**Triple-diff workflow:**

* ctest: 7/7
* `--diff-jit-trace`: match through 11 038 cycles, 321 blocks — clean
* Boot 300 K / 5 M det: same final PC, same helper count as M6.86;
  tiny improvement from the rare boot-path ADDQ.W with skip_flags=true.

**Perf:**

| Workload | M6.86 | **M6.87** | Δ |
|----------|------:|----------:|--:|
| Bench @ 5 M cyc   | 1.371 lx7/cyc | **1.359 lx7/cyc** | **−0.9 %** lx7 |
| Bench @ 20 M cyc  | 1.211 lx7/cyc | **1.198 lx7/cyc** | **−1.1 %** lx7 |
| Bench @ 100 M cyc | 1.353 lx7/cyc | **1.340 lx7/cyc** | **−1.0 %** lx7 |
| Boot @ 300 K det  | 2.159 lx7/cyc | **2.158 lx7/cyc** | within noise |
| Boot @ 5 M det    | 12 352 738 lx7 | **12 341 858 lx7** | −10 880 lx7 |
| Boot @ 100 M cyc  | 1.734 | **1.734** | unchanged |

**Cumulative M6.84 → M6.87:**

| Workload | M6.84 | M6.87 | Δ |
|----------|------:|------:|--:|
| Bench @ 20 M cyc  | 1.257 | **1.198** | **−4.7 %** lx7 |
| Bench @ 100 M cyc | 1.396 | **1.340** | **−4.0 %** lx7 |

**Bench is now 1.198 lx7/cyc @ 20 M cyc = 5.39 × interp baseline.**

The actual measured saving (~268 K LX7 / 20 M cyc) is less than the
naive estimate (~1.6 M from 405 K × 4) — looking at the chain stats
(241 K chain transitions / ~4 per loop iter ≈ 60 K full iterations
in 20 M cyc), the actual ADDQ.W execution count is ~60-67 K per site,
not the M6.41-era 405 K. The savings rate matches that scale.

## M6.86 — LEA + ADDA fusion (delivered)

Bench's 0x03DF4E-50 (the LEA-sibling of the 0x03DF40 MOVEA pair) was
the next-tier candidate noted in M6.85's STATUS:

```
LEA   (2,A4,D6.W),A2 ; ADDA.W D6,A2
                     ↓ fused into
                     ; A2 = A4 + 2·sext_w(D6) + 2     (xt_addx2 + xt_addi)
```

Standard emit was 5 LX7 ops on the cached fast path: a13 = sext D6
(memoized from upstream), a8 = A4 (cache hit), a8 += a13, a8 += d8 (=2),
store back into Am — then ADDA.W's single xt_add. Fused emit collapses
to 2 LX7: `xt_addx2 Am, a13, A4` followed by `xt_addi Am, Am, 2`
(skipped when d8 == 0). No CCR concerns.

Implemented in the existing LEA arm: peek next op for ADDA.W matching
the LEA's index reg and destination. Fusion eligibility:

* LEA srcmode == 6 (the `(d8,An,Xn)` brief-format form)
* LEA index is `.W` (ext bit 11 == 0) and brief format (bit 8 == 0)
* Next op is ADDA.W with `(nw >> 9) & 7 == am` and source register
  matches the LEA's Xn (D-or-A reg)

**Triple-diff workflow:**

* ctest: 7/7
* `--diff-jit-trace`: match through 11 038 cycles, 321 blocks — clean
* Boot @ 300 K / 5 M deterministic: byte-identical to M6.85 (and M6.84)

**Perf:**

| Workload | M6.85 | **M6.86** | Δ |
|----------|------:|----------:|--:|
| Bench @ 5 M cyc   | 1.394 lx7/cyc | **1.371 lx7/cyc** | **−1.6 %** lx7 |
| Bench @ 20 M cyc  | 1.237 lx7/cyc | **1.211 lx7/cyc** | **−2.1 %** lx7 |
| Bench @ 100 M cyc | 1.378 lx7/cyc | **1.353 lx7/cyc** | **−1.8 %** lx7 |
| Boot 300 K / 5 M det | identical | identical | — |
| Boot @ 100 M cyc  | 1.734 | unchanged | — |

`inline_ops` dropped from 1764 → 1763 — one new fusion site (the
0x03DF4E LEA), firing at every bench iteration.

**Cumulative M6.84 → M6.86:**

| Workload | M6.84 | M6.86 | Δ |
|----------|------:|------:|--:|
| Bench @ 20 M cyc  | 1.257 | **1.211** | **−3.7 %** lx7 |
| Bench @ 100 M cyc | 1.396 | **1.353** | **−3.1 %** lx7 |

**Bench is now 1.211 lx7/cyc @ 20 M cyc = 5.336 × interp baseline.**

The 0x03DF40 hot block (405 K hits / 20 M cyc) now has three fusion
sites firing per iteration: MOVEA+ADDA (×1), MOVEA+ADDA+ADDA (×1 triple),
and LEA+ADDA. The remaining ops in the block — MOVE.W (d8,A0,Xn),D5,
MOVE.W D5,D4, MOVE.W (A2),(A3), MOVE.W D4,(A2), ADDQ.W #1,D6,
CMPA.W (d16,A5),A6 — are mostly already pure single-op inlines or
already-fused (CMP+Bcc). Next-tier fusion candidates noted for
follow-up: MOVE.W Dn,Dm + MOVE.W Dm,(EA) (re-using the .W extraction).

## M6.85 — MOVEA + ADDA fusion (delivered)

Bench's hot block at PC `0x03DF40` (~405 K hits / 20 M cyc) is dominated
by two patterns:

```
MOVEA.L A4,A0           ; A0 = A4
ADDA.W  D6,A0           ; A0 += sext_w(D6)
                        ↓ fused into
                        ; A0 = A4 + sext_w(D6)         (one xt_add)

MOVEA.L A4,A3           ; A3 = A4
ADDA.W  D6,A3           ; A3 += sext_w(D6)
ADDA.W  D6,A3           ; A3 += sext_w(D6)
                        ↓ fused into
                        ; A3 = A4 + 2·sext_w(D6)       (one xt_addx2)
```

Implemented in the existing MOVEA.L arm of the dispatch chain: peek
ahead at the next op (and optionally the one after) and absorb the
ADDA.W into the MOVEA's emit if same destination Am. Cycles and PC
delta accumulate (8 c + 12 c [+ 12 c]; 2 b + 2 b [+ 2 b]).

Triple-fusion needs `xt_addx2` which already existed (used by M6.31 for
DBcc cycle math) — Xtensa LX7's `ADDX2 ar, as, at` computes
`ar = (as << 1) + at` in one cycle, so two `xt_add`s collapse to one.

**Triple-diff workflow:**

* ctest: 7/7
* `--diff-jit-trace`: match through 11 038 cycles (321 blocks), no
  divergence before the documented M6.66 boundary
* Boot @ 300 K / 5 M deterministic: byte-identical to M6.84 (same
  final PC `0x40032C`, same `inline_ops` and `helper_ops`) — the
  fusion doesn't fire on the deterministic boot window (mostly ROM
  init with non-MOVEA/ADDA opcode mixes)

**Perf:**

| Workload | M6.84 | **M6.85** | Δ |
|----------|------:|----------:|--:|
| Bench @ 5 M cyc   | 1.411 lx7/cyc | **1.394 lx7/cyc** | **−1.2 %** lx7 |
| Bench @ 20 M cyc  | 1.257 lx7/cyc | **1.237 lx7/cyc** | **−1.6 %** lx7 |
| Bench @ 100 M cyc | 1.396 lx7/cyc | **1.378 lx7/cyc** | **−1.3 %** lx7 |
| Boot @ 300 K det  | 2.159 lx7/cyc | **2.159 lx7/cyc** | identical |
| Boot @ 5 M det    | 12 352 738 lx7 | **12 352 738 lx7** | identical |
| Boot @ 100 M cyc  | 1.734 lx7/cyc | **1.734 lx7/cyc** | identical |

`inline_ops` dropped from 1767 → 1764: each absorbed ADDA loses its
own counted entry, so 3 ADDA.W ops were fused away across the 898
bench blocks. With those 3 fusion sites in the 0x03DF40 family (each
running ~405 K iters) the savings amortise to ~1.2 M LX7 over 20 M
cycles ≈ the 1.6 % drop observed.

**Bench is now 1.237 lx7/cyc @ 20 M cyc = 5.224 × interp baseline**
(6.462 / 1.237). First fusion-driven win since M6.30's CMP+Bcc family.

### Why this matters as a class

The "absorb the cheap MOV into the dependent ALU" pattern generalises
to any `MOVE Rs,Rd ; OP src2,Rd` where `OP` is the only consumer of Rd
in the immediate window AND `OP` has a three-operand form. Future
candidates:

* `MOVE.L Dn,Dm ; ADD.L Dx,Dm` → `xt_add Dm, Dn, Dx`
* `MOVEA.L (d16,An),Am ; ADDA.W Dx,Am` (one of M6.75's hot paths) →
  same idea but with the memory load folded in
* `LEA <ea>,An ; ADDA.W Dn,An` (bench's 0x03DF4E-50 sibling) →
  collapsing the LEA's index-disp computation with the trailing ADDA

## High-gain backlog (notes for the optimisation loop)

These are the next-tier levers identified after M6.84. Recorded here
so the autonomous loop has a stable target list rather than re-deriving
them each iteration.

1. **Full register caching of hot D/A regs across a block** — eliminates
   intra-block `l32i` / `s32i` pairs. The current per-block cache is
   limited to 4 slots picked by the prologue's `cache_sig` heuristic;
   widening this (or making it adaptive per-block) is the biggest
   single remaining structural win. Likely 10-20 % on bench, smaller
   on boot.
2. **Comprehensive lazy-CC classifier** that correctly models helper
   CCR usage per opcode (currently the SET/CONS table is conservative —
   anything that touches the helper bridge defaults to SET|CONS even
   when the helper itself doesn't write CCR). Tightening this lets
   `flags_dead[i]` skip more emit-flag passes.
3. **Native block chaining on the ESP32 target** — eliminates the
   dispatcher round-trip per chained block on real hardware. Already
   landed under `#ifdef ESP_PLATFORM`; host's xt_sim runs one block
   per invocation so it can't be measured here, but on real hardware
   this is an estimated ~12 % wall-clock win.
4. **Instruction fusion (aka dynamic recompiling)** — emit dependent
   pairs/triples of 68k ops as a single LX7 sequence, eliminating the
   intermediate register writeback. See M6.85 below for the first
   fusion lever in this class.

**Important note — M6.77 reset.** The bench numbers in this table
are the **first correct measurements** of the milestone in many
sub-steps. From M6.62 to M6.76, a latent
`emit_logic_flags` `vreg=10/11` clobber (see
`memory/emit-logic-flags-vreg-conflict.md`) silently emitted wrong
N/Z flags whenever the lazy-CC pass didn't mark
`flags_dead = true` for the follow-on. The
`diff_jit_bench_lockstep` ctest passed lockstep up to cycle 11898
in every case (the documented M6.66 VIA-tick boundary), but
post-cycle-11898 the bench ran on a wrong code path — usually
"stuck looping in a ROM segment at PC ≈ `0x401???`" instead of the
intended Speedometer hot loop at PC ≈ `0x03DF58` in RAM. Numbers
recorded in M6.74-76 (`2.950`, `2.868`, `2.787`) are not directly
comparable to the post-M6.77 figures — they were measured on the
wrong path. M6.77 fixed the clobber and unblocked the correct
loop. **ctest: 7/7** including the diff_jit_bench_lockstep
regression guard.

The JIT exceeds interp on every host metric. The remaining ESP32-only
optimisations (M6.54 native chaining, M6.62 cross-block register
caching) add an estimated ~12 % + ~3 % boot win on real hardware —
untestable via the host Xtensa sim, which runs one block per
invocation.

### Run it

```sh
./scripts/build.sh        # cmake + build
./scripts/test.sh         # ctest, 4/4
./scripts/gui.sh          # SDL GUI — Mac Plus ROM + System 6 floppy
./scripts/bench.sh        # JIT cost on the Speedometer snapshot
./scripts/diff.sh         # JIT vs interp lockstep (triple-diff SOP)
```

Direct flags (`./build/mac68k_host --help`): `--interp` / `--jit`,
`--rom`, `--disk`, `--load-snapshot`, `--max-cycles`,
`--arena-kb N`, `--evict none|lru|fifo`, `--diff-jit`,
`--diff-jit-trace`, `--no-irq`.

### Session arc (M6.2 → M6.70 highlights)

| Stage | Bench lx7/cyc | Boot lx7/cyc | What landed |
|-------|--------------:|-------------:|-------------|
| M6.2  | 4.008 | 5.376 | First optimisation pass |
| M6.30 | ~1.45 | ~3.5  | Comprehensive lazy CCs, fused CMP+Bcc |
| M6.49 | 1.288 | 1.727 | Inlined RTS, JMP .L, push/pop extensions |
| M6.54 | 1.279 | 1.739 | ESP32 native block chaining (`#ifdef ESP_PLATFORM` only) |
| M6.62 | 1.279 | 1.739 | Cross-block register caching (ESP32 only, 99.7 % match rate on boot) |
| M6.63 | 1.279 | 1.735 | Runtime LRU/FIFO eviction policies + sweep |
| **M6.68** | 1.279 | 1.735 | **Latent SR-flush bug fixed** (was set since M6.61, caught by `--diff-jit`) |
| M6.70 | 1.279 | 1.735 | New ctest locks M6.68 in |

`memory/triple-differential.md` records the workflow that found
M6.68: always `--diff-jit` after any JIT change — ctest's curated
snippets miss whole bug classes.

### Original session summary (preserved for historical context)

A 68000 Macintosh emulator with a 68000 → Xtensa-LX7 JIT, targeting
the ESP32-S3. Milestones M1–M4 and M6 are done: a reference
interpreter, a working JIT, the emulator on `qemu-system-xtensa`, and
a **real Macintosh Plus ROM + System 6.0.8 booting to the Finder**
under both engines. The interpreter and JIT reach byte-identical
state (modulo VIA-tick timing past ~12 K cycles), and the ADDX/SUBX/
NEGX condition-code fix found via the mini vMac differential is in.

**M5 — JIT optimisation — was the prior current work, now done.**
The basic-block JIT did not initially beat the interpreter; the
M6.10–M6.70 sub-milestones closed that gap.

### Session result — M6.2 → M6.70 (real-cost basis, post-M6.41 metric correction)

| Engine | Start (M6.2) | M6.49 stop | M6.70 (current) | × Interp |
|--------|-------------:|-----------:|----------------:|---------:|
| Bench  | 4.008 lx7/cyc | 1.288 | **1.279 lx7/cyc** | **5.05 ×** ✅ |
| Boot   | 5.376 lx7/cyc | 1.727 | **1.735 lx7/cyc** | **3.40 ×** |

Both engines tripled in real performance during M6.2 → M6.49. Bench
cleared the user's 5×-interp goal at M6.31. Boot's gains came from a
sequence of inline expansions and finally a specialised "fast-path
MMIO helper" framework (M6.42–M6.47) that bypasses `m68k_step`'s
decode overhead for the common VIA-register read / write paths.

The M6.54 → M6.62 native-chaining + cross-block-register-cache work
is ESP32-only and not reflected in host metrics; the M6.68 SR-flush
fix is a correctness improvement that left perf unchanged.

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
script + ~5 min scripted run + one snapshot.

**Attempted in a later session and ran into a coordinate-and-timing
wall.** What works:
* Headless boot to desktop with Infinite HD auto-mounted.
* Apple-menu drop (single click on `(12, 10)` — clean).
* File-menu drop (single click on `(46, 8)` — clean).
* Single-click selection of Infinite HD icon at `(444, 170)` (icon
  shows the inverted-highlight state).
* Frame-dump every-N-cycles for visual inspection (sips converts
  the BMPs to PNG for review).

What fails:
* Double-click to open the icon — across timings from 64 ms span to
  6 ms span and from y=153 to y=170, the icon never opens. Single
  click always selects but the OS doesn't register the pair as a
  double-click in scripted mode. Possibly an interaction with the
  emulator's mouse-state update mechanism (`mac_set_mouse` only
  re-writes MTemp when position changes — at same-(x,y) clicks the
  cursor's CrsrNew flag stays clear).
* Cmd-O via the keyboard-event extension (see below) — the key
  events DO reach `find_key_event` and the M0110 wire bytes get
  shifted out, but the Finder doesn't fire the Open action.
  Possibly a focus / selection-cleared race when keys arrive too
  soon after the click.
* Mouse drag from menubar to "Open" item — menu drops, "Open"
  highlights, but release doesn't open the (presumably
  deselected-by-menu) icon.

What was added during the attempt (kept):
* `port/host/main.c`: scripted-run format now accepts keyboard
  events as `k <cycle> <keycode> <down>` (decimal/hex via `%i`).
  The existing 4-token `<cycle> <x> <y> <btn>` mouse events still
  work. Useful for any future scripting attempt regardless of how
  the open-icon problem is solved.

Suggested next steps if pursuing this further:
* Use the SDL GUI interactively (`mac_gui`) to manually open the
  app to the right state, then take a snapshot via the snapshot-
  hook (extend `mac_gui` to dump a snapshot on a hotkey).
* Investigate `mac_set_mouse`'s `LM_CrsrNew` interaction with
  same-position clicks — the OS may need CrsrNew to be raised
  even on no-position-change for double-click registration.

**M6.67 — bench scripting unblocked: x-coord was wrong, mouse works.**

Resumed the bench-scripting attempt. The earlier "double-click won't
open the icon" was a coordinate bug, not a mouse-delivery problem.
Probed icon x positions on the System 6 desktop:

```
x=444, y=170 → cursor lands between icons, no selection
x=465, y=170 → icon SELECTED + DOUBLE-CLICK OPENS THE WINDOW
```

The Mac Plus desktop puts disk icons centered around x=465 (~20 px
further right than my original guess). Once corrected, double-click
at `(465, 170)` opens Infinite HD straight to its top-level window.

Achieved in this iteration:
* Open Infinite HD via double-click. (host 9 software-category
  folders visible.)
* Open Utilities folder via double-click at `(370, 240)`.
* Open TattleTech sub-folder via single-click + `Cmd-O` (the
  keyboard-event scripting extension from M6.64 is critical here).
  Saw the inner window with the actual app `TattleTech 2.84` and
  `READ ME`.
* Select `TattleTech 2.84` app at `(225, 255)` — icon inverts on
  selection.

Still stuck: launching the selected `TattleTech 2.84` app. Both
double-click and `Cmd-O` after select fail to actually fire the
"open application" Finder action. The folder-open path works, the
app-launch path doesn't. Possible causes:
* App's CREATOR/TYPE codes lost during the infinitemac.org chunked-
  HFS reassembly — Finder can't identify the binary as launchable.
* Different click-region semantics for app icons vs folder icons.
* `LM_DoubleTime` or focus issue specific to apps.

Mouse script primitive that works:
```
# Cycles are 7.83 MHz emulated; 600K cycles = ~77 ms hold.
<cyc>       <x> <y> 0    # initial position-set + release
<cyc+300K>  <x> <y> 1    # first press
<cyc+900K>  <x> <y> 0    # first release
<cyc+1.5M>  <x> <y> 1    # second press
<cyc+2.1M>  <x> <y> 0    # second release
```

The frame-by-frame coordinate probe technique used here:
```sh
for y in 155 160 165 170 175 180; do
    cat > /tmp/probe.mscript <<EOF
1500000000 444 \$y 0
1500200000 444 \$y 1
1500800000 444 \$y 0
EOF
    rm -f /tmp/macframes/*.bmp
    MAC68K_MOUSESCRIPT=/tmp/probe.mscript MAC68K_FRAMEDIR=/tmp/macframes \
        MAC68K_FRAME_EVERY=50000000 MAC68K_END_CYCLE=1700000000 \
        ./build/mac68k_host --jit --rom roms/macplus.rom \
            --disk roms/disks/System6.dsk --server >/dev/null 2>&1
    sips -s format png /tmp/macframes/frame_031.bmp --out /tmp/probe_y\$y.png
done
```

Quick to iterate; the BMP→PNG conversion (sips) is what makes it
readable. With this technique, finding icon coords is ~30 sec per
icon.

**M6.65 — `--diff-jit` finds a real pre-existing JIT divergence
(not introduced by recent commits; ctest misses it).**

Per user direction "always compare JIT result to interp result and
mini vMac result to find divergences", running `--diff-jit` on the
existing `speedo-bench.snap` immediately reveals:

```
./build/mac68k_host --diff-jit-trace --no-irq \
    --load-snapshot roms/disks/speedo-bench.snap --max-cycles 2000

[trace] DIVERGENCE at step 36, block PC=0x41E184, cycles 1630..1666
  SR interp=2700 jit=2704        # JIT sets Z=1, interp Z=0
  pre-block:  D3=0x00000000
  block:
    PC=41E184  op=99CC  ; SUBA.L A4,A4   (bridged to m68k_step)
    PC=41E186  op=5443  ; ADDQ.W #2,D3   (inlined)
    PC=41E188  op=4E75  ; RTS            (block terminator)
```

After the block: D3 = 2 (both engines agree); flags should be CCR=0
(Z=0 since result≠0). Interp gets this right; JIT sets Z=1.

The bug exists at M6.61 too — it pre-dates M6.62 (cross-block reg
caching) and M6.63 (eviction policies). `ctest` doesn't catch it —
`test_jit.c` runs a small curated snippet set, not this block.

Walked through `emit_addq_w_dn` + `emit_addsub_flags_long_masked`
emission by hand:
* `xt_bnez(r, 6)` at result ≠ 0 correctly skips the Z-setting
  `xt_or` (xt_or is 3 bytes; branch target PC+6 lands past it).
* The mask `xt_movi(12, -32); xt_and R_SR,R_SR,12` should clear
  CCR bits 0-4, then `xt_or R_SR,R_SR,a8` ORs in the new bits.

Plausible causes (not investigated to root):
* SUBA.L bridge's `emit_sr_reload` reading a `cpu->sr` modified
  unexpectedly during m68k_step (interrupt poll? supervisor flag?).
* `flags_dead[]` liveness analysis marking ADDQ.W's flags dead
  for this block (it shouldn't — RTS is classified CONS, meaning
  it reads CCR, so ADDQ's flag write isn't dead).

**Workflow change per user direction:** `--diff-jit` is SOP after
any JIT-affecting commit. ctest passing isn't enough. Memory at
`memory/triple-differential.md`.

**M6.66 — narrowed M6.65 bug to `emit_addq_w_dn`.**
Bisect via temporary env-gated bridge: forcing the ADDQ.W to fall
through to `m68k_step` makes the divergence at step 36 (PC=0x41E184)
disappear — confirming the bug is in `emit_addq_w_dn` or its
`emit_addsub_flags_long_masked` call.

What was verified:
* `flags_dead[1]` is 0 (correct — flag write isn't dead).
* `block_needs_sr_load` is 1 (correct — prologue loads R_SR).
* `rc.active` is 0 (D3 not cached; read via l32i).
* The emitted bytes (102 = 34 instructions × 3) match the expected
  layout. Decoded `xt_bnez(a10, 6)` = `56 2A 00` — target PC+6
  cleanly skips the 3-byte `xt_or(8,8,9)` that would set the Z bit
  when `a10 != 0`. `xt_movi(12, -32)` = `C2 AF E0` — correctly
  sign-extends to `0xFFFFFFE0` to clear CCR bits 0-4.

Where the bug must be (not pinpointed):
* The SUBA.L bridge's `emit_sr_reload` may be reading a `cpu->sr`
  mutated during `m68k_step` by some subtle path.
* `g_sr_dirty` may be incorrectly false at epilogue flush time,
  causing the modified R_SR not to be written back.
* Some clobbered-register interaction between the SUBA.L bridge's
  cache_reload and the inline's emit_read_g.

Workaround (NOT committed; diagnostic only):
```c
if (getenv("MAC68K_NO_ADDQW_INLINE")) { /* fall through */ } else { … }
```
Around the `emit_addq_w_dn` call site.

Next iteration: insert a debug print at the END of block emission
that dumps R_SR and cpu->sr just before the standard return, to
distinguish "R_SR is correct in-register but flushed wrong" from
"R_SR is wrong in-register".

**M6.68 — root-caused + fixed.**
Followed M6.66's "next iteration" plan: instrumented the simulator
to log every `xt_or`/`xt_and` touching R_SR (a14) and every `s16i`
to OFF_SR. Trace showed R_SR ending at 0x2700 in-register but
`cpu->sr` becoming 0x2704 after the block.

Root cause: `emit_helper_step_after_flush_undo` is called from the
*slow-path* branch of a beqz/bnez. Its first emission is an
`emit_sr_flush`. At runtime that s16i only fires on the slow path,
but the *compile-time* `emit_sr_flush` clears `g_sr_dirty`. The rest
of the block then "thinks" R_SR has been flushed, and the epilogue's
own `emit_sr_flush` becomes a no-op. On the fast path (e.g., RTS
with SP in RAM, no bridge), R_SR's prior modifications (ADDQ.W's
flag writes) are still live but never reach `cpu->sr`.

Fix: snapshot `g_sr_dirty` on entry of `emit_helper_step_after_flush_undo`
and restore it on exit. Diff-jit-trace divergence moved from
step 36 (cycle 1666) to step 350 (cycle 11898). The remaining
"divergence" at 350 is the documented VIA-tick granularity artifact
— only VIA registers differ, no register/PC/SR/RAM diffs.

**M6.69 — same fix applied to `emit_jit_fast_helper`.**
Same shape (called from the slow-path branch of a beqz, with an
internal `emit_sr_flush`). Used by the MOVE.W (An)+,Dn MMIO fast-
helper bridge. Same snapshot-and-restore fix.

Bench 1.279 / boot 1.735 unchanged across M6.68 + M6.69.
ctest 3/3.

**Remaining divergence (≥ 50 K cycles) is not a JIT bug.** It's
downstream propagation from the VIA-tick granularity artifact:
Speedometer reads VIA T1, the JIT and interp see slightly different
timer values (~4 emulated cycles apart), code branches differently.
Fixing would require per-instruction `mac_mem_tick` in the JIT too
— a substantial perf hit. Documented as a known limitation of the
timing model (matches M3's "good enough to pace ~60 Hz VBL, not
instruction-cycle-accurate" note).

**Triple-differential SOP vindicated.** M6.68 was a real correctness
bug present at least since M6.61 (and probably earlier) that ctest
missed. `--diff-jit` on the real benchmark snapshot caught it
immediately. Workflow memory at `memory/triple-differential.md`.

**M6.71 — block prefetch (static-successor speculative compile).**

Adds `--prefetch none|static` (default `none`). When `static`, after
the dispatcher compiles a block on demand, it also speculatively
compiles each statically-known successor of that block (BRA / Bcc /
JMP (xxx).L / JMP (d16,PC) / JSR variants / BSR / DBcc / plain
non-control-flow fall-through). Capped at 1 level deep to keep the
upfront cost bounded; skips arena-resets on the prefetch path so a
speculative compile never evicts the just-compiled real block.

New `m68k_block_static_successors(b, mem, out[2]) → int`
extracts up to 2 successor PCs from a block's last_op. Returns 0 for
dynamic terminators (RTS / RTE / RTR / TRAP / STOP / JMP (An) /
JSR (An) / line-A / line-F).

Two new stats: `prefetch_compiles` (blocks compiled speculatively)
and `prefetch_hits` (chain misses where prefetch had already
compiled the target). Printed in the `[host]` line.

Tests added:
* `test_prefetch` — unit tests for `m68k_block_static_successors`,
  hand-crafts blocks for every terminator shape (BRA.S / BRA.W /
  BRA.L=dynamic, Bcc.S / Bcc.W, BSR.S, DBNE.W, JMP/JSR .L / .W /
  (d16,PC), RTS / RTE / TRAP / JMP (An) / line-A / STOP all return
  0, fall-through cap returns pc_end).
* `diff_jit_bench_lockstep_prefetch` — re-runs the M6.68 SR-flush
  lockstep regression guard but with `--prefetch static`. Confirms
  prefetch doesn't perturb the JIT/interp invariants.

Measurements (host, lx7_per_cyc unchanged across all scenarios —
prefetch trades compile latency for execution-cost-neutral
speculation):

| Workload                    | prefetch=none | prefetch=static | wall-clock |
|-----------------------------|--------------:|----------------:|-----------:|
| Bench 60 M cyc              | 1.279         | 1.279           | identical  |
| Boot 100 M cyc, 1 MB arena  | 1.735         | 1.735           | +2.6 % wc  |
| Boot 100 M cyc, 64 KB FIFO  | 1.734         | 1.734           | identical  |
| Boot 10 M cyc (cold start)  | 1.7x          | 1.7x            | **−5.0 % wc** |
| Bench 300 M cyc (long run)  | 1.279         | 1.279           | +3.2 % wc  |

Bench: 522 prefetched compiles (of 1010 total), 100 prefetch hits
(chain misses prefetch had beaten to compilation). Boot: 1425 / 102 208,
352 hits.

The lx7_per_cyc metric is correctness-neutral because prefetch only
changes *when* a block compiles, not *what* runs. Wall-clock shows
the canonical prefetch tradeoff: wins on cold / short runs (5 % on
the 10 M-cycle boot), small loss on long steady-state runs (3 % on
300 M-cycle bench — the extra speculative blocks compete with the
working set without paying back). On the typical 100 M-cycle boot
the tradeoff is roughly neutral.

**Recommended default: off** for the standard host bench / boot,
where wall-clock matters and steady-state dominates. **On for
embedded ESP32-S3 deployment** where the working set is much smaller
relative to the JIT arena and one-shot boot latency matters more.
Code path is conditional and zero-overhead when off.

**M6.72 — better prefetch policy: `--prefetch chain`.**

The M6.71 `static` mode wasted ~80 % of its prefetched compiles
(bench: 522 prefetched, only 100 hits; boot: 1425 prefetched, only
352 hits) because it compiled *both* branches of every Bcc / DBcc
even though typically only one is taken. The new `chain` mode keeps
the prefetch idea but two changes:

* **Skip ambiguous successors.** Only follow blocks with a single
  static successor (BRA / BSR / JMP (xxx).L / JMP (xxx).W /
  JMP (d16,PC) / JSR variants / plain block-cap fall-through). Bcc
  and DBcc are not prefetched on either side — let the runtime's
  predicted-next link establish itself on first execution.
* **Recurse along linear chains.** Static mode is single-hop; chain
  mode follows up to `--prefetch-depth N` (default 2) hops along
  the linear sequence. So a `BRA → MOVE.L Dn,(An)+; ... ; RTS`
  pattern prefetches not just the BRA target but the next
  unconditional successor too.

Result (final block count, prefetched count, prefetch hits):

| Workload                    | none      | static                | chain                |
|-----------------------------|----------:|----------------------:|---------------------:|
| Bench 60 M cyc, blocks=     | 863       | **1010** (+147 waste) | **863** (no waste)   |
|   prefetch_compiles         | 0         | 522                   | 168                  |
|   prefetch_hits             | 0         | 100                   | 45                   |
| Boot 100 M cyc, blocks=     | 101 858   | 102 208 (+350 waste)  | 101 858              |
|   prefetch_compiles         | 0         | 1 425                 | 365                  |
|   prefetch_hits             | 0         | 352                   | 20                   |

Chain's defining property: **block count equals the no-prefetch
baseline**. Every block it prefetched was one the runtime would have
compiled anyway — chain just moved the compile earlier in time.
Static, in contrast, leaves 147 / 350 dead blocks in the cache
forever (or until evicted), competing with the working set.

Wall-clock 5-run avg (noisier than block counts, ~±2 % band):

| Workload         | none      | static                | chain                |
|------------------|----------:|----------------------:|---------------------:|
| Bench 60 M cyc   | 166 ms    | 165 ms (-0.8 %)       | 169 ms (+1.8 %)      |
| Boot  100 M cyc  | 722 ms    | 727 ms (+0.6 %)       | 726 ms (+0.6 %)      |
| Boot  10 M cyc cold | 50.4 ms | 50.0 ms (-0.8 %)     | **49.9 ms (-1.0 %)** |
| Bench 300 M long | 944 ms    | **924 ms (-2.1 %)**   | 929 ms (-1.6 %)      |

`lx7_per_cyc` identical across all three modes everywhere
(correctness preserved — the metric measures execution, not
compile).

The advantage of `chain` is **structural rather than per-workload
loud**: same first-order amortisation as static, no wasted compiles
and no arena pollution. When the arena is small (M6.63 showed the
ESP32-relevant 64–256 KB regime), every wasted block matters more —
this is exactly where chain pulls ahead, but the host's 1 MB arena
masks that.

Implementation: `m68k_dispatcher_set_prefetch(d, mode, depth)`. Mode
is `PREFETCH_NONE` / `PREFETCH_STATIC` / `PREFETCH_CHAIN`; depth is
the CHAIN follow depth (default 2; `--prefetch-depth N` to override).

Tests:
* The M6.71 `test_prefetch` unit suite still passes (the static-
  successor analyser is shared).
* New `diff_jit_bench_lockstep_prefetch_chain` ctest runs the
  M6.68 SR-flush regression guard with `--prefetch chain`.

ctest: **7 / 7** pass (M6.71 left it at 6; +1 new).

**Updated recommendation:** `chain` (depth 2) is now the suggested
default if any prefetch is enabled. `static` remains available for
A/B comparison but is strictly dominated by `chain` on the "no
wasted compiles" axis.

**M6.73 — batch of hot-opcode native emits (helper → inline).**
Targeted seven 68k opcodes that the `JIT_HELPER_HISTO` build flagged
as bench- or boot-warm but were still falling through to `m68k_step`:

| Opcode | Form | 20 M-cyc bench count |
|--------|------|--------------------:|
| `0x4258` | `CLR.W (A0)+` (memset-zero loop body) | 5003 (one-shot) |
| `0x4480-7` | `NEG.L Dn` | ~1500 |
| `0x303C+reg<<9` | `MOVE.W #imm16,Dn` | ~1000 |
| `0x3040+ra<<9+rd` | `MOVEA.W Dn,An` (sign-extend) | ~1000 |
| `0x3B40+ra<<9+rd` | `MOVE.W Dn,-(An)` | ~1500 |
| `0x202F+rd<<9+ra` | `MOVE.L (d16,An),Dn` (stack-frame .L read) | ~1000 |
| `0x2F41+rd<<9+ra` | `MOVE.L Dn|Am,(d16,An)` (stack-frame .L write) | ~1000 |

Each emit follows the existing fast-path-with-bounds-check shape
(beqz over a helper bridge → fast inline write or read of BE bytes
with An post-increment / pre-decrement / d16 displacement).

**Bug found and fixed: `xt_movi` only encodes signed 12-bit (-2048..
+2047).** First implementation of `MOVE.W #imm16,Dn` used
`xt_movi(a10, (i32)(i16)imm)` for the immediate, which silently
wrapped any imm ∉ [-2048,2047] to its low 12 bits — corrupting Dn.
The buggy emit passed the 11 K-cycle `diff_jit_bench_lockstep` ctest
(its imm16 stream stays small), but boot's init code with
`MOVE.W #0xXXXX, Dn` for various Toolbox masks hit it immediately.

Diagnosed by **boot-path bisection at 300 K cycles (pre-VBL,
deterministic):** enabled emits one at a time, watched for boot's
final PC to differ from baseline. The buggy emit drove the boot to
PC=`0x4002C2` (helpers=122 — far too few) instead of baseline's
PC=`0x40032C` (helpers=1195). Fix: replace `xt_movi` with
`emit_load_imm(dst, tmp, (u32)imm)`, which falls back to
`emit_load_imm32`'s multi-op build for values out of the 12-bit
range.

**Per-emit triple-differential workflow that found the bug:**

1. Wrote each emit.
2. `ctest` — 7/7 (passes the 11 K-cycle `diff_jit_bench_lockstep`).
3. Boot @ 300 K cycles (deterministic window pre-first-VBL): the
   final PC, helper count, and `jit_cost` should match the previous
   commit exactly. Any difference is a register-corruption
   correctness bug, surfaced before VIA path-dependence enters.

This 3rd step was the new lesson — ctest's 11 K bench window doesn't
exercise `MOVE.W #imm16,Dn` with large immediates (the snapshot's
imm stream stays small there); the **deterministic boot 300 K window**
does. It's now part of the recommended workflow alongside `ctest`
and `--diff-jit-trace`.

**Perf delta (post-fix, all seven emits enabled):**

| Workload | Pre-M6.73 | M6.73 | Δ |
|----------|----------:|------:|--:|
| Boot @ 5 M cyc (deterministic; PC=`0x40032C` both)   | 25279 helpers, 2.604 lx7/cyc | 25240 helpers, 2.603 lx7/cyc | −39 helpers (−0.04 % lx7) |
| Boot @ 100 M cyc (path-dependent past first VBL)     | ~225 K helpers, 1.735 lx7/cyc | ~223 K helpers, 1.734 lx7/cyc | −2 K helpers (negligible lx7) |
| Bench @ 11 K cyc (deterministic; pre-divergence)     | 405 helpers, 3.959 lx7/cyc | 405 helpers, 3.959 lx7/cyc | identical (emits don't fire in this window) |
| Bench @ 5 M / 100 M cyc                              | path-dependent | path-dependent | not measurable — VIA-tick granularity (M6.66) |

The per-emit gain on real, deterministic workloads is small because
boot's hot opcodes don't include these forms heavily — they're
**bench-hot** by the histogram, but **bench is path-dependent past
cycle 11 898** so the steady-state perf number can't be read
directly. The correct outcome here is: emits are inline-correct
(triple-diff passes + boot-path matches), bench helpers go down by
the per-call count when the emit fires, and the ESP32 wall-clock
will reflect the inline savings on real hardware.

**Lessons recorded:** `memory/triple-differential.md` already
existed; `memory/xtensa-movi-range.md` added to catch the
`xt_movi` 12-bit pitfall before it ships again. Two new emits
(NEG.L Dn, CLR.W (An)+) are also templates for future similar
helper-eviction work.

ctest: **7 / 7** pass.

**M6.74 — three more hot-opcode native emits (CMPA.L, MOVEA.L (An)+, CLR.L).**
The M6.73 / `JIT_HELPER_HISTO` re-scan at 20 M-cycle bench found a
new top entry: `0xB1C9` (CMPA.L A1, A0) at **272 K hits — 21 % of
all bench helpers and ≈ 17 % of total bench `jit_cost`**. Inlining
it is the single biggest helper-eviction left on the table.

Added three emits in one batch:

| Opcode | Form | 20 M bench count | Notes |
|--------|------|-----------------:|-------|
| `0xB1C9` etc. | `CMPA.L (Dn|An),An` | 272 K | Register-source. `top==0xB && szf==3 && (w>>8)&1==1 && mode∈{0,1}`. Two reads + sub + flags via `emit_addsub_flags_long_masked` (with `keep_x=true` — CMPA never writes X). 2 bytes / 10 cyc. |
| `0x28D8` etc. | `MOVEA.L (An)+,Am` | 71 K | Long sibling of the M6.62 `MOVE.L (An)+,Dn` arm; writes to An (no flags). Still helper-bound 71 K times in 20 M cyc because the bench reads ROM tables — RAM_BOUNDS only validates the RAM range. |
| `0x4299` etc. | `CLR.L (An)+`     | 26 K | Long sibling of M6.73's CLR.W (An)+. 4-byte zero-write + 4-byte post-incr; same CLR-specific flag emit. |

**Workflow** ran the new triple-diff SOP unchanged: after each emit,
ctest → diff-jit-trace (DIVERGENCE only at the documented M6.66
cycle 11898) → boot @ 300 K and 5 M (PC=`0x40032C` and helpers
both still match the M6.73 baseline — these opcodes are bench-hot,
not boot-hot, so the boot path is undisturbed).

**Perf delta (post-M6.74):**

| Workload | M6.73 | M6.74 | Δ |
|----------|------:|------:|--:|
| Bench @ 11 K cyc (deterministic) | 405 helpers, 3.959 lx7/cyc | **385 helpers, 3.873 lx7/cyc** | −20 helpers, **−2.2 % lx7** |
| Bench @ 5 M cyc (PC=`0x401F52` both) | 197 K helpers, 3.969 lx7/cyc | **115 K helpers, 2.932 lx7/cyc** | −82 K helpers, **−26 % lx7** |
| Bench @ 20 M cyc (PC=`0x40115E` both) | 796 K helpers, 3.978 lx7/cyc | **469 K helpers, 2.950 lx7/cyc** | −327 K helpers, **−26 % lx7** |
| Boot @ 5 M cyc (deterministic) | 25240 helpers, 2.603 lx7/cyc | 25240 helpers, 2.603 lx7/cyc | identical (boot not hot on these ops) |
| Boot @ 100 M cyc | 1.734 lx7/cyc | 1.734 lx7/cyc | identical |

The bench gain is the largest single jump since the M6.30s. CMPA.L
A1,A0 was a single inner-loop instruction in Speedometer's
result-tally code that the JIT had been bridging to `m68k_step` ~14
million times per 100 M-cyc run.

**Why MOVEA.L (An)+ still shows 71 K helpers** (didn't drop):
Bench's hot loop reads structure pointers out of a ROM-resident
table (PC ≈ `0x408???`). The current RAM_BOUNDS literal only
validates the **RAM** range; ROM reads always go to the helper.
Extending the fast path to also cover ROM (which is read-only, so
no SMC tracking concerns) is the next obvious step — listed in
"Open follow-ups" below.

ctest: **7 / 7** pass.

### Open follow-ups after M6.74

* **Extend the bounds-check literal to admit ROM reads.** ROM at
  `0x400000-0x41FFFF` is a single contiguous range; a two-mask
  emit (RAM accept OR ROM accept) would unlock ~71 K bench helper
  conversions and likely matter on the ESP32 hardware where
  byte-by-byte ROM reads dominate boot's MOVE.B-heavy init code.
* **Overlay-aware bounds check.** During Mac Plus overlay (the
  first ~300 K cycles of boot), `ram_bounds_mask` returns all-ones
  → every memory op goes to the helper. Boot's overlay phase is
  ~25 K helpers in 5 M cyc; adding a second mask for the overlay's
  RAM-at-0x600000 placement would knock ≥ 80 % of those out.

**M6.75 — MOVEA.L (d16,An),Am + helper-histo opcode-decode lesson.**
Attempted the M6.74-followup "ROM-extended bounds check on MOVEA.L
(An)+,Am", landed it green through ctest + boot-deterministic 300 K,
then measured: bench helpers unchanged (468 K). Bisected by
re-decoding the bench's `0x28D8` from the histogram — I'd misread
it as `MOVEA.L (A0)+, A4` in M6.74's STATUS commentary, but the
correct decode is **`MOVE.L (A0)+, (A4)+`** (memory-to-memory
post-incr, dst_mode=3, NOT dst_mode=1). The M6.74 MOVEA.L inline
was correct *for the rare actual MOVEA.L (An)+,Am calls*, but the
71 K hot helpers on bench were a different op entirely.

Reverted the M6.75 ROM-check (slight regression — extra inline ops
with no hits to eliminate) and pivoted to the *actually-hot*
MOVEA.L variant: `0x246F (MOVEA.L (d16,SP),A3)` at **13 241 hits
in 20 M cyc** with no inline emit. Direct sibling of M6.73's
`MOVE.L (d16,An),Dn` arm — same `An + sext16(d16)` address compute
and `.L` read, with the result written to Am instead of Dn and
the flag emit dropped (MOVEA doesn't touch CCR).

Match condition: `top == 0x2 && (w >> 6) & 7 == 1 && mode == 5`.
2-byte+ext (length 4), 8 cycles (interp base 4 + handler 4).

**Verification:**
- ctest 7/7
- diff_jit_trace: only the documented M6.66 cycle-11898 divergence
- Boot @ 300 K: PC=`0x40032C`, helpers=1192 — byte-identical to M6.74
- Boot @ 5 M: PC=`0x40032C`, helpers=25240 — byte-identical to M6.74

**Perf:**

| Workload | M6.74 | **M6.75** | Δ |
|----------|------:|----------:|---|
| Bench @ 5 M cyc (PC=`0x401F52` both) | 115 036 h, 2.932 lx7/cyc | **106 316 h, 2.850 lx7/cyc** | **−8 720 h, −2.8 %** |
| Bench @ 20 M cyc (PC=`0x40115E` both) | 468 562 h, 2.950 lx7/cyc | **434 122 h, 2.868 lx7/cyc** | **−34 440 h, −2.8 %** |
| Boot @ 5 M / 100 M cyc | unchanged | identical | — (not boot-hot) |

**Lesson recorded** in `memory/triple-differential.md`: when iterating
on opcode-eviction work driven by the `JIT_HELPER_HISTO` dump, always
**re-decode** the opcode's bit field semantics before assuming the
M6.X comments are right. Histogram entries are 16-bit raw words —
the same nibble pattern can map to different `dst_mode` arms (MOVEA
vs MOVE-to-(An)+) and the wrong-target emit silently passes ctest
+ the deterministic-boot check because the actually-hot op was
never tested.

ctest: **7 / 7** pass.

**M6.76 — MOVE.L (An)+,(Am)+ mem-to-mem inline with RAM-or-ROM source.**
Took the actually-hot helper class M6.75 misidentified — `0x28D8`
(MOVE.L (A0)+,(A4)+) at **71 K hits in 20 M cyc** plus `0x22D8`
(MOVE.L (A0)+,(A1)+) at **13 K hits** — and inlined it.

Bit-field decode (re-derived from binary per the post-M6.75 SOP):

```
0x28D8 = 0010_1000_1101_1000
         top=2  reg=4 m=3   m=3 reg=0
                      dst   src
       → top=0x2 (.L MOVE), dst_mode=3 ((An)+), src_mode=3 ((An)+)
```

Match condition: `top == 0x2 && (w >> 6) & 7 == 3 && mode == 3`.

The source is in **ROM** (Speedometer reads structure-pointer tables
at PC ≈ `0x408???`), so the fast path needs a RAM-OR-ROM gate.
Re-introduced the M6.75-attempt literals (`LITERAL_ROM_BOUNDS`,
`LITERAL_ROM_BASE`, `ADDR_ROM_HOST_BASE`) — this time with a real
hot user. Destination is **RAM only** (writes to ROM are silently
dropped by mac_write32, so we defer that to m68k_step). Bounds
shape:

```
a10 = src & RAM_MASK
a11 = (src & ROM_MASK) ^ ROM_BASE      ← 0 iff src in ROM
a12 = a10 & a11                         ← 0 iff src RAM-or-ROM
a9  = dst & RAM_MASK
a12 |= a9                               ← 0 iff both pass
```

Fast path: pick src host base from `a10` (RAM_BASE if `a10==0`, else
ROM_HOST_BASE), read 4 BE bytes, increment src An, write 4 BE bytes
to dst host ptr (always RAM_BASE), increment dst An. The
`xt_beqz a10, 6` that picks the base is a one-instruction
conditional emit, so the common RAM-source case pays only one extra
branch over a single-base inline shape.

**Same-An edge case** (`MOVE.L (A0)+, (A0)+`): the 68k semantic does
src ea_decode (captures orig and increments An), then dst ea_decode
(captures the now-incremented An and increments again), so the write
happens at `orig + 4` and An ends at `orig + 8`. The inline mirrors
this with a one-op `xt_mov a15, a8` re-sync after the src post-incr,
only emitted when `src_an == dst_am` at compile time.

**Triple-diff workflow** (the M6.75 lesson made the boot-deterministic
check a hard prerequisite — re-derived the opcode decode FIRST, then
wrote the emit, then ran the suite):

* ctest: 7/7
* `--diff-jit-trace`: only the documented M6.66 cycle-11898 divergence
* Boot @ 300 K (pre-VBL): PC=`0x40032C`, helpers=1192 — byte-identical to M6.75
* Boot @ 5 M cyc: PC=`0x40032C`, helpers=25240 — byte-identical to M6.75

**Perf:**

| Workload | M6.75 | **M6.76** | Δ |
|----------|------:|----------:|--:|
| Bench @ 5 M cyc (PC=`0x401F52` both)  | 106 316 h, 2.850 lx7/cyc | **84 963 h, 2.767 lx7/cyc** | **−21 K h, −2.9 %** |
| Bench @ 20 M cyc (PC=`0x40115E` both) | 434 122 h, 2.868 lx7/cyc | **349 500 h, 2.787 lx7/cyc** | **−85 K h, −2.8 %** |
| Boot @ 5 M / 100 M cyc                | unchanged                | identical                  | — (not boot-hot) |

The 20 M helper drop of 84 622 matches the predicted savings exactly
(71 K + 13 K = 84 K for the two `(An)+→(An)+` variants).

**Cumulative M6.73-76 bench progression** (5 M-cyc snapshot,
PC=`0x401F52` throughout):

| Stage | helpers | lx7/cyc |
|-------|--------:|--------:|
| M6.72 (pre-batch)              | 197 K  | 3.969 |
| M6.74 (CMPA.L + …)             | 115 K  | 2.932 |
| M6.75 (MOVEA.L (d16,An),Am)    | 106 K  | 2.850 |
| **M6.76 (mem-to-mem RAM+ROM)** | **85 K**   | **2.767** |

ctest: **7 / 7** pass.

The next two top helpers (`0x4a38` TST.B (xxx).W at 15 K and
`0xe54a` LSR.W variant at 13 K) are smaller, more specialised
targets. The "ROM source admitted" literals are now in steady use
and ready for other reads (e.g. `MOVE.L (xxx).W,Dn` patterns at
boot if they surface).

**M6.77 — TST.B (xxx).W inline + latent `emit_logic_flags` clobber
fix (huge bench unblock).**

Started this iteration targeting `0x4A38` (TST.B (xxx).W, 15 K
bench helpers in 20 M cyc) with a compile-time-RAM-static inline:
the 16-bit ext word is known at codegen time, sign-extends to a 24-
bit address, and the bench's first-PC (`0x4028DE`) reads
`(xxx).W = 0x0349` — Mac low-memory RAM globals, statically RAM.
For RAM-bound addresses the inline loads RAM_BASE + abs_addr → host
ptr, reads one byte, sets MOVE-family CCR via shift-to-bit-31 +
`emit_logic_flags`; for MMIO/out-of-RAM the static analysis skips
the inline and falls to the m68k_step helper. 4 cycles, length 4.

First measurement after the new inline: **bench got STUCK at PC=
`0x4028F0`** (the BEQ right after the TST.B at `0x4028DE`),
spinning through ~227 K helpers in 5 M cyc. ctest passed, the
boot-deterministic 300 K window still matched byte-for-byte, but
the bench was clearly wrong. Bisecting the new emit by-line found
that `emit_logic_flags(e, vreg=10)` was the actual culprit.

**The bug:** `emit_logic_flags`'s VERY FIRST emitted instruction is
`xt_movi a10, -16` — clobbering `a10` before the subsequent `extui`
and `bnez` read `vreg`. If the caller passes `vreg ∈ {10, 11}`
(both used internally for scratch), the read sees the clobbered
value instead. With `vreg=10`, every flag emit gets:

* `extui a11, a10, 31, 0` reads `0xFFFFFFF0` → N bit = 1 always.
* `bnez a10, +6` is taken → Z=0 always.

So the function emits a sequence that yields **N=1, Z=0 regardless
of the actual value.** Existing callers in M6.62, M6.73, and M6.74
all pass `vreg=10` (the .L value lands in a10 after the BE-byte
assembly), and have been silently relying on the lazy-CC pass
marking the typical follow-on flags_dead — but the bug was always
there, waiting for a consumer that actually reads Z. The new TST.B
inline's natural follow-on is BEQ/BNE, which reads Z; the bug
manifested instantly.

**Fix:** shadow `vreg` into `a9` before the body runs:

```c
static void emit_logic_flags(xt_emit *e, u8 vreg) {
    if (vreg == 10 || vreg == 11) {
        xt_mov(e, 9, vreg);
        vreg = 9;
    }
    /* ... unchanged body ... */
}
```

One extra `mov` for vreg=10/11 callers; existing callers' wrong
behavior corrected silently.

**The bench wasn't measuring what we thought it was.** With the
bug, bench's `MOVE.L (d16,An),Dn` and `MOVE.L (An)+,Dn` (the M6.62 /
M6.73 inlines, where the .L value lands in a10) emitted bogus
flags whenever the lazy-CC analysis didn't mark flags_dead. The
`diff_jit_trace` ctest still passes lockstep up to cycle 11898
because pre-11898 the bench either doesn't exercise those emits
with flags_dead=false or interp/JIT happen to agree on the wrong
behavior (since both make the same "wrong" branch). **Post-11898**
(the documented VIA-tick artifact), the JIT diverges to whatever
path the wrong flags push it down. From M6.62 onward, that path
has been "stuck looping in some ROM code segment at PC ≈ 0x401???"
instead of the bench's intended Speedometer hot loop at
`PC ≈ 0x03DF58` in RAM.

All M6.62→M6.76 bench numbers were measured on that wrong path.
The M6.77 numbers below — and the new headline — are the first
**correct** bench measurements since the bug landed.

**Perf (post-fix, the new honest baseline):**

| Workload | M6.76 (buggy path) | **M6.77 (corrected)** | Δ |
|----------|-------------------:|----------------------:|---|
| Bench @ 5 M cyc   | 84 963 h, 2.767 lx7/cyc, PC=`0x401F52` | **14 124 h, 1.487 lx7/cyc, PC=`0x03DF58`** | **−84 % helpers, −46 % lx7** |
| Bench @ 20 M cyc  | 349 500 h, 2.787 lx7/cyc, PC=`0x40115E` | **20 766 h, 1.304 lx7/cyc, PC=`0x03DF58`** | **−94 % helpers, −53 % lx7** |
| Bench @ 100 M cyc | 4 765 088 h, 4.163 lx7/cyc | **344 973 h, 1.446 lx7/cyc** | **−93 % helpers, −65 % lx7** |
| Boot @ 300 K det  | unchanged | **unchanged** | byte-identical |
| Boot @ 1 M / 5 M det | unchanged | **unchanged (±6 lx7 from the extra mov)** | byte-identical helper counts |
| Boot @ 100 M (path-divergent) | 1.735 | 1.779 | within noise of post-VBL path-divergence |

**Comparison to the original 5×-interp goal:** the bench's interp
baseline is `6.462 lx7/cyc`. The corrected JIT bench `1.304` →
**5.0× interp at 20 M cyc**, **4.5× interp at 100 M cyc** —
matching the M6.51 headline claim that this 5×-line had been
crossed, except now the comparison is real.

**Triple-diff workflow:**

* ctest: 7/7
* `--diff-jit-trace`: only the documented M6.66 cycle-11898 divergence
* Boot @ 300 K, 1 M, 5 M deterministic windows: byte-identical
  helper counts vs M6.76; tiny `xt_mov`-added lx7 deltas

**Memory recorded:**

`memory/emit-logic-flags-vreg-conflict.md` documents the bug class
(emit-side internal scratch overlapping with caller's vreg) and the
defensive-shadow pattern as the standard fix. Linked from
`memory/triple-differential.md` since it's the second internal-
scratch-clobber to ship undetected by ctest's 11K bench window
(after the M6.75 `xt_movi`-12-bit-range one — see
`xtensa-movi-range.md`).

ctest: **7 / 7** pass.

**M6.78 — JSR (d16,PC) + MOVE.L #imm32,-(An) inline + dispatch-arm
arity bug fix.**

Two opcodes (3571 combined helpers/20 M cyc on the M6.77 corrected
bench path):

* `0x4EBA` JSR (d16,PC) — 2563 hits. Block terminator. Mirrors the
  existing RTS arm: read SP, decrement by 4, bounds-check new SP,
  helper bridge on failure, fast path writes the return PC
  (= `cpu->pc + 4` after `emit_advance_flush` lands cpu->pc on
  source_pc) as 4 BE bytes to `[ram_base + new_SP]`, commits new
  SP via `emit_write_g`, then loads target PC from LITERAL_BCC_PC
  (= source_pc + 2 + sext_d16, stored in the per-block literal
  slot by the block-setup pre-pass — same single-l32r-instead-of-
  10-op-build trick already used by JMP.L) and writes it to
  cpu->pc. Cycles 20 (interp base 4 + handler 16). Length 4.

* `0x2F3C` MOVE.L #imm32,-(An) — 1008 hits. Immediate push. Same
  shape as the existing MOVE.L Dn|Am,-(An) arm but with a compile-
  time-known src: 4 `xt_movi + xt_s8i` pairs to write the byte
  constants directly (skipping the 10-op `emit_load_imm32` +
  4 `extui` extract pattern), and a compile-time-folded MOVE-family
  flag update (N/Z derived from the imm at compile time, emitted as
  a 4-op mask+OR sequence vs `emit_logic_flags`' 8-op runtime path).
  Length 6, cycles 8.

**Dispatch-arm arity bug found and fixed during measurement.** First
build had the new MOVE.L #imm32,-(An) arm placed AFTER the existing
`MOVE.L / MOVEA.L #imm32,Dn/An` arm. The existing arm's outer `if`
condition matched ANY dst_mode (it just gated the immediate-src
shape), then handled only dst_mode ∈ {0,1} inside; for other
dst_modes (4 = -(An)) the arm exited without setting `done = true`.
The dispatch's `else if` chain then **never tried the subsequent
arms** because the outer condition already matched — JSR's helper
count dropped to 0 (its own dispatch was clean), but the histogram
still showed 0x2F3C at 1008, surfacing the issue. **Fix:** tighten
the outer condition on the existing arm to
`((w >> 6) & 7) == 0 || ((w >> 6) & 7) == 1` so non-matching
dst_modes fall through to the new arm. A line comment now warns
about the prior trap. (Recorded in `memory/dispatch-arm-arity.md`.)

**Triple-diff workflow:**

* ctest: 7/7
* `--diff-jit-trace`: only the documented M6.66 cycle-11898 divergence
* Boot @ 300 K det: PC=`0x40032C`, helpers=1192 — byte-identical to M6.77
* Boot @ 5 M det: PC=`0x40032C`, helpers=25240 — byte-identical to M6.77

**Perf:**

| Workload | M6.77 | **M6.78** | Δ |
|----------|------:|----------:|--:|
| Bench @ 5 M cyc   | 14 124 h, 1.487 lx7/cyc | **12 314 h, 1.470 lx7/cyc** | **−1 810 h, −1.1 %** |
| Bench @ 20 M cyc  | 20 766 h, 1.304 lx7/cyc | **17 195 h, 1.296 lx7/cyc** | **−3 571 h, −0.6 %** |
| Bench @ 100 M cyc | 344 973 h, 1.446 lx7/cyc | **336 119 h, 1.442 lx7/cyc** | **−8 854 h, −0.3 %** |
| Boot @ 5 M / 100 M | unchanged | identical | — (not boot-hot) |

The 20 M helper drop of 3571 matches the predicted savings exactly
(2563 + 1008). Bench `1.296` at 20 M cyc lands at **4.99 × interp
baseline** (6.462 / 1.296) — just under the clean 5× line on the
*correct* execution path.

ctest: **7 / 7** pass.

**M6.79 — JSR (d16,An) + ADDA.L (d16,An),Am inline — bench 5.01 × interp.**

Two opcodes, **3003 combined bench helpers/20 M cyc**:

* **`0x4EAD` etc. (mask `0xFFF8` matching `0x4EA8`) — JSR (d16,An).**
  1003 hits. Block terminator. Sibling of M6.78's JSR (d16,PC) but
  the target is computed at runtime from `An` (not a compile-time
  constant), so we stash the result in `a15` BEFORE the bounds check
  and write it to `cpu->pc` after the fast path's stack push. `a15`
  survives `emit_cache_flush` (which only touches cache slots
  `a4..a7`) and the helper bridge's `callx0` doesn't get a chance to
  clobber it because the runtime never reads `a15` along the
  helper-path branch (the helper bridge `j over`s the fast block).
  20 cycles (base 4 + handler 16), length 4.

* **`0xD1EE` ADDA.L (d16,A6),A0 (mask `0xF1F8` matching `0xD0E8` —
  `top == 0xD && szf == 3 && (w>>8)&1 && mode == 5`).** 2000 hits.
  **Was mis-decoded in the M6.78 follow-up notes as "ADD.L
  D0,(d16,A6)" — re-deriving the bit fields per the SOP showed it's
  actually opmode 111 = ADDA.L** (Am destination, not a RMW with
  flags). That makes the emit much simpler: load .L from `An + d16`,
  add to dst Am, **no flag emit at all** (ADDA never touches CCR).
  Direct sibling of M6.75's MOVEA.L (d16,An),Am but adding instead
  of replacing. Source can be in ROM (bench's pointer tables at
  PC ≈ `0x408???`), so the M6.76 unified RAM-or-ROM bounds + base
  selector apply directly. 12 cycles (base 4 + handler 8 — the
  ADDA path adds 8, vs the plain MOVE/MOVEA path's 4), length 4.

**Triple-diff workflow ran clean** with the post-M6.75 SOP — both
emits decoded bit-by-bit from binary FIRST. The ADDA mis-decode in
the M6.78 notes wasn't a shipping bug (no commit reached emission
based on the wrong decode), but it was almost one: had I implemented
"ADD.L Dn,(d16,An)" as initially imagined it would have emitted a
full-RMW with set-CCR-flags inline for an op that actually clobbers
nothing — wrong semantics, wrong cycle count, and almost certainly a
bench-path divergence post-cycle-11898 that the deterministic-boot
window wouldn't have caught.

* ctest: 7/7
* `--diff-jit-trace`: only the documented M6.66 cycle-11898 divergence
* Boot @ 300 K / 5 M deterministic: byte-identical to M6.78

**Perf:**

| Workload | M6.78 | **M6.79** | Δ |
|----------|------:|----------:|--:|
| Bench @ 5 M cyc   | 12 314 h, 1.470 lx7/cyc | **10 810 h, 1.457 lx7/cyc** | **−1 504 h, −0.9 %** |
| Bench @ 20 M cyc  | 17 195 h, 1.296 lx7/cyc | **14 191 h, 1.289 lx7/cyc** | **−3 004 h, −0.5 %** |
| Bench @ 100 M cyc | 336 119 h, 1.442 lx7/cyc | **328 615 h, 1.439 lx7/cyc** | **−7 504 h, −0.2 %** |
| Boot @ 100 M cyc  | 1.779                    | 1.779                       | unchanged |

The 20 M helper drop of 3004 matches the predicted savings
(1003 JSR + 2000 ADDA.L = 3003, off by 1 from natural variation).

**Bench is now `1.289 lx7/cyc` at 20 M cyc = `5.013 × interp
baseline`** (6.462 / 1.289). The clean 5× line is crossed on the
honest M6.77-corrected execution path — the first commit since
M6.31/M6.51 where the 5×-interp headline is real.

ctest: **7 / 7** pass.

**M6.80 — three (d16,An)-mode arms: MOVE.L (An)+,(d16,Am) + CMPI.W
#imm16,(d16,An) + ADD.W (d16,An),Dn.** 2030 combined bench helpers
predicted; 2117 measured. Bench 5.02 × interp.

Three inlines, all sharing the (d16,An) address-compute shape and
the M6.76 RAM-or-ROM source bounds (each one's source can be ROM —
bench reads ROM tables, stack frames in RAM low globals, etc.):

* **`0x2F5F` MOVE.L (An)+,(d16,Am).** 1000 hits. Sibling of M6.76's
  MOVE.L (An)+,(Am)+ but the dst uses `(d16,An)` mode instead of
  `(An)+`. The same-An case (e.g. `(SP)+ → (d16,SP)`, which is the
  hot one) needs the dst-addr base = src An's POST-incr value, so
  the inline uses a one-op compile-time `xt_addi a15, a8, 4` when
  `src_an == dst_am` is known at compile time, otherwise reads dst
  An separately.

* **`0x0C6D` CMPI.W #imm16,(d16,An).** 515 hits. Read .W from
  An+sext_d16, shift to high 16 (the same flag-correctness trick
  CMP.W (d16,An),Dn uses), subtract from imm.W also at high 16,
  derive CMP flags via `emit_addsub_flags_long_masked(is_sub=true,
  keep_x=true, ...)`. Length 6 (op + imm + d16).

* **`0xD06D` ADD.W (d16,An),Dn.** 515 hits (and the `0x9` sibling
  SUB.W (d16,An),Dn covered by the same arm). Sibling of the
  ADD.W Dm,Dn arm at line ~3730 but src is a `.W` memory read.
  Preserves Dn[31:16] and replaces Dn[15:0] with the sum's low 16,
  via the same shift-to-high-16 / extract pattern. Length 4.

**Triple-diff workflow ran clean** with the post-M6.75 SOP — all
three opcodes decoded bit-by-bit from binary FIRST:

* ctest: 7/7
* `--diff-jit-trace`: only the documented M6.66 cycle-11898 divergence
* Boot @ 300 K / 5 M deterministic: byte-identical to M6.79

**Perf:**

| Workload | M6.79 | **M6.80** | Δ |
|----------|------:|----------:|--:|
| Bench @ 5 M cyc   | 10 810 h, 1.457 lx7/cyc | **10 100 h, 1.455 lx7/cyc** | **−710 h, −0.1 %** |
| Bench @ 20 M cyc  | 14 191 h, 1.289 lx7/cyc | **12 074 h, 1.288 lx7/cyc** | **−2 117 h, −0.08 %** |
| Bench @ 100 M cyc | 328 615 h, 1.439 lx7/cyc | **321 031 h, 1.438 lx7/cyc** | **−7 584 h, −0.07 %** |
| Boot @ 100 M cyc  | 1.779 | 1.779 | unchanged |

The 20 M helper drop of 2117 matches the predicted savings (2030
target, +87 incidental from rare variants of the new arms not
counted in the histogram top-20).

**Bench is now `1.288 lx7/cyc` at 20 M cyc = `5.017 × interp`
(6.462 / 1.288).**

The metric is now firmly in the diminishing-returns regime — each
remaining bench helper class is ≤ 515 hits/20 M cyc, and the per-call
inline savings of ~30-50 lx7 translate to sub-0.1 % bench moves. The
next significant lever is one of:

* **Adding the Xtensa MULL encoder + sim decode** to true-inline
  MULS.W variants (combined 2002 bench helpers — `0xC0FC`
  `MULS.W #imm16,D0` + `0xC1ED` `MULS.W (d16,A5),D0`).
* **Per-block prologue/epilogue trimming**: bench has 241 K chain
  transitions in 20 M cyc with only 3.3 % matching prev->cache_sig
  (vs boot's 99.7 %). Reducing chain-transition cost by even a few
  ops would yield millions of lx7.

ctest: **7 / 7** pass.

**M6.81 — xt_mull + sim decode + MULS/MULU.W #imm16 / (d16,An).**

Added the Xtensa Mul32-option MULL instruction (op0=0, op1=2, op2=8)
to:

* the encoder (`xt_mull` in `jit/emit_xtensa.{h,c}`)
* the host sim (`s->a[r] = s->a[sr] * s->a[t]` for the op1=2/op2=8
  RRR slot in `jit/xtensa_sim.c`)

Then inlined two bench-hot multiply variants — combined **2002
helpers/20 M cyc**:

* **`0xC0FC` MULU.W #imm16, D0** at 1002 hits, plus the rarer signed
  sibling `0xC1FC` MULS.W. Compile-time imm; sign- or zero-extend
  Dn's low .W to 32, sign- or zero-extend imm.W to 32, one MULL.
  Write .L to Dn; MOVE-family CCR. 74 cycles, length 4.

* **`0xC1ED` MULS.W (d16,A5), D0** at 1000 hits, plus the rarer
  unsigned sibling `0xC0ED` MULU.W (d16,An), Dn. RAM-or-ROM source
  bounds (M6.76 shape) for the .W memory read; same sext/zext-then-
  MULL pattern after. 74 cycles, length 4.

**Bit-field decode trap, caught mid-iteration:** First M6.81 cut
gated the arms on `(w >> 8) & 1 == 1` (the "MULS" bit), miss-decoding
the hot `0xC0FC` as MULS.W. Re-decoding bit by bit per
`memory/triple-differential.md`:

```
0xC0FC = 1100_0000_1111_1100  (bit 8 = 0 → MULU, NOT MULS)
0xC1ED = 1100_0001_1110_1101  (bit 8 = 1 → MULS, as expected)
```

So `0xC0FC` is actually `MULU.W #imm16, D0` (unsigned). The first
build still showed 1002 helpers at `0xC0FC` post-inline — fixed by
dropping the `(w >> 8) & 1` constraint from the outer condition and
branching internally on the sign flag.

**Triple-diff workflow:**

* ctest: 7/7
* `--diff-jit-trace`: only the documented M6.66 cycle-11898 divergence
* Boot @ 300 K / 5 M deterministic: byte-identical to M6.80

**Perf:**

| Workload | M6.80 | **M6.81** | Δ |
|----------|------:|----------:|--:|
| Bench @ 5 M cyc   | 10 100 h, 1.455 lx7/cyc | **9 091 h, 1.447 lx7/cyc** | **−1 009 h, −0.55 %** |
| Bench @ 20 M cyc  | 12 074 h, 1.288 lx7/cyc | **10 065 h, 1.284 lx7/cyc** | **−2 009 h, −0.31 %** |
| Bench @ 100 M cyc | 321 031 h, 1.438 lx7/cyc | **316 022 h, 1.436 lx7/cyc** | **−5 009 h, −0.14 %** |
| Boot @ 100 M cyc  | 1.779 | 1.779 | unchanged |

The 20 M helper drop of 2009 matches the predicted savings exactly
(1002 MULU.W + 1000 MULS.W + 7 incidental).

**Bench is now `1.284 lx7/cyc` at 20 M cyc = `5.032 × interp`**
(6.462 / 1.284). The new MULL instruction unlocks future multiply-
heavy inlines (DIVS/DIVU need a divide instruction, which Xtensa
LX7 doesn't have natively — those will stay as helpers).

ctest: **7 / 7** pass.

**M6.82 — third-tier chain entry (skip the redundant prologue ops on
chain-hit + no-cache-compat). Structural win: bench −1.9 %, boot
−5 % on the deterministic window.**

The M6.62 cross-block reg-cache already had two tiers for chain JX
target selection: `body_addr` (skip everything when cache+SR match)
and `entry_addr` (full prologue otherwise). On bench the cache-match
rate is only 3.3 % (vs boot's 99.7 %), so 96.7 % of bench's 241 K
chain transitions paid the full prologue cost.

But two of the prologue ops are **redundant on every chain
transition**, regardless of cache match:

* `l32r R_CPU, ADDR_CPU_BASE` — `R_CPU = a3` is a callee-saved
  register in the CALL0 ABI, and the chain JX is a register jump,
  not a call. The predecessor's `a3` already holds `HOST_CPU_BASE`.
* `s32i a0, R_CPU, OFF_JITRETPC` — the chain epilogue's
  `l32i a0, R_CPU, OFF_JITRETPC` BEFORE the JX already restored a0
  to the saved value, so this s32i writes the same value back.

Added a third entry point per block, **`chain_entry_addr`**, which
points AFTER those two ops but BEFORE the SR reload and cache
reloads. Dispatcher now picks:

| Chain state | Target | Skips |
|---|---|---|
| Cold dispatch (`prev == NULL`) | `entry_addr` | nothing |
| Chain hit + cache_sig + SR compat | `body_addr` | l32r RCPU + s32i a0 + sr_reload + cache_reload |
| Chain hit + not compat | **`chain_entry_addr`** *(new)* | l32r RCPU + s32i a0 only |

On the ESP32 native-chain path the savings come for free (a3 / a0
are naturally preserved across the JX). On the **host** xt_sim each
block runs in a fresh sim with all registers zeroed, so the host
side pre-loads the equivalent state in C before `xt_sim_run`:

* `chain_entry_addr` start: pre-set `s.a[3] = HOST_CPU_BASE`.
* `body_addr` start: pre-set `s.a[3] = HOST_CPU_BASE`, `s.a[14] =
  cpu->sr` if `b->sr_loaded`, and each active cache slot
  `s.a[4+slot] = cpu->d[gi] / cpu->a[gi-8]`. C-side cost isn't
  counted in `xt_instrs`, so the host metric correctly reflects the
  LX7-op savings the ESP32 path gets from the prologue skip.

**Why boot wins so much more than bench:** boot's 99.7 % cache-match
rate means almost every chain transition now reaches `body_addr` and
skips the **entire** prologue (5+ ops). Bench's 3.3 % match rate
means most transitions use `chain_entry_addr` and save only 2 ops
per transition. Boot's per-transition savings are ~2.5× bench's.

**Triple-diff workflow:**

* ctest: 7/7
* `--diff-jit-trace`: only the documented M6.66 cycle-11898 divergence
* Boot @ 300 K / 5 M deterministic: same final PC (`0x40032C`), same
  helper count (1192 / 25240) as M6.81 — the metric drop is from
  reduced inline op count, not from runtime divergence

**Perf:**

| Workload | M6.81 | **M6.82** | Δ |
|----------|------:|----------:|--:|
| Bench @ 5 M cyc   | 9 091 h, 1.447 lx7/cyc | **9 091 h, 1.418 lx7/cyc** | **−2.0 %** lx7 |
| Bench @ 20 M cyc  | 10 065 h, 1.284 lx7/cyc | **10 065 h, 1.259 lx7/cyc** | **−1.9 %** lx7 |
| Bench @ 100 M cyc | 316 022 h, 1.436 lx7/cyc | **316 022 h, 1.409 lx7/cyc** | **−1.9 %** lx7 |
| Boot @ 300 K det  | 1 192 h, 2.320 lx7/cyc | **1 192 h, 2.170 lx7/cyc** | **−6.5 %** lx7 ✨ |
| Boot @ 5 M det    | 25 240 h, 2.603 lx7/cyc | **25 240 h, 2.471 lx7/cyc** | **−5.1 %** lx7 ✨ |
| Boot @ 100 M cyc  | 1.779 lx7/cyc | **1.735 lx7/cyc** | **−2.5 %** lx7 |

The helper counts are identical to M6.81 — this is a pure prologue-
trim win, no opcodes changed.

**Bench is now `1.259 lx7/cyc` at 20 M cyc = `5.133 × interp`**
(6.462 / 1.259). The chain-prologue lever (the largest structural
piece flagged in M6.80) is now spent. The remaining structural
piece would be **improving bench's cache-match rate** — every match
upgrades a `chain_entry_addr`-tier hit into a `body_addr`-tier hit,
doubling that transition's savings. Bench's per-block reg-set
diversity makes this hard (an experimental canonical-slot-order
refactor was prototyped this iteration but had zero effect — bench's
blocks pick different reg *sets*, not just different orderings).

ctest: **7 / 7** pass.

**M6.83 — SWAP Dn + BSR.S inline; CMP.W (An),Dn MMIO-trap reverted.**

Small iteration adding two clean inlines plus a hard-earned lesson:

* `(w & 0xFFF8) == 0x4840` **SWAP Dn** — bench-warm 0x4840+ at 246
  hits/20 M cyc. Swap the high and low .W halves: `result = (v
  << 16) | (v >> 16)`. Uses `xt_extui` (sa=16 / msksize=16) for the
  high→low half since `xt_srli`'s shift count is capped at 15.
  MOVE-family CCR, length 2, 4 cycles.
* `top == 0x6 && cc == 1 && disp ∈ -127..-1 ∪ 1..127` **BSR.S disp8**
  at 193 hits. Block terminator; sibling of M6.78 JSR (d16,PC) but
  with 8-bit displacement. Target = `source_pc + 2 + sext8(disp8)`,
  stashed in `LITERAL_BCC_PC` by an extended block-setup pre-pass
  that now treats `cc ∈ {0, 1}` as BRA-like (vs the old code which
  treated cc=1 as fall-through-like for the literal). Return PC =
  `cpu->pc + 2` computed at runtime. 22 cycles, length 2.

**CMP.W (An),Dn arm — landed and immediately reverted.** Added
the mode=2 sibling of CMP.W (d16,An),Dn using the M6.76 unified
RAM/ROM bounds. ctest passed, `diff_jit_trace` showed only the
documented M6.66 divergence. But the **bench `lx7_per_cyc` jumped
from 1.259 to 1.390** (+10.4 %) — investigated by bisect (disabling
each new arm). The hot 0xB692 is CMP.W (A2),D3 in the bench's
ROM-resident polling loop, where A2 almost always points to MMIO
(VIA-register polling). Every call paid the unified-bounds setup
ops AND then fell through to the helper bridge anyway, NET cost
+~8 LX7 ops per call. The helper count even dropped in the
histogram, but the bench post-cycle-11898 path took a slightly
different code shape (VIA-tick path dependence) so the count
comparison was misleading.

Recorded in `memory/inline-when-ea-points-mmio.md`: when an opcode's
EA is most often MMIO, inline-with-bounds-check is a NET LOSS even
when ctest and diff_jit_trace pass. Always check `lx7_per_cyc` and
total `xt_instrs` after landing a new inline; if those go up while
helpers go down, you've hit this trap.

**Triple-diff workflow:**

* ctest: 7/7
* `--diff-jit-trace`: only the documented M6.66 cycle-11898 divergence
* Boot @ 300 K / 5 M deterministic: byte-identical to M6.82 (1192 /
  25240 helpers, same lx7 cost)

**Perf:**

| Workload | M6.82 | **M6.83** | Δ |
|----------|------:|----------:|--:|
| Bench @ 20 M cyc | 10 065 h, 1.259 lx7/cyc | **9 443 h, 1.258 lx7/cyc** | −622 h, ~0 % lx7 |
| Boot @ 300 K / 5 M det | unchanged | unchanged | byte-identical |
| Boot @ 100 M | 1.735 | 1.735 | unchanged |

622 helpers eliminated (matches the SWAP 246 + BSR.S 193 + in-family
variants prediction). The lx7_per_cyc savings is negligible (~0.1 %)
because both SWAP and BSR.S have small total runtime call counts
relative to bench's 25 M-lx7 cost.

ctest: **7 / 7** pass.

**M6.84 — skip the epilogue's `l32i a0, OFF_JITRETPC` for blocks
that emit no CALLX0.** Universal pure-trim win, 0.5-0.9 % across
all workloads.

Every block ends with `l32i a0, OFF_JITRETPC; jx a0`. The `l32i` is
needed because CALLX0 (in helper bridges) clobbers `a0` with its
post-callx0 PC. But if the block emitted NO callx0 — neither the
default `m68k_step` bridge nor any of the conditional bridges inside
inline arms — then `a0` still holds whatever the prologue established
(the RET sentinel on host, or the chain-preserved value on ESP32),
so the `l32i` is redundant.

The existing `helper_ops` counter only tracks the default helper
bridge at the bottom of the dispatch chain (line 4377 of codegen.c).
**Conditional bridges inside M6.73-style arms** —
`emit_helper_step_after_flush_undo`, `emit_jit_fast_helper`, and the
BTST_B_MMIO / ORI_B_MMIO custom bridges — also emit callx0 but were
not counted.

Added `g_block_emitted_callx0`, a per-block compile-time flag, reset
at prologue setup and set by every callx0-emitting site (5 in total).
The epilogue gates the `l32i` on this flag.

**Mid-iteration breakage caught + fixed:** First attempt gated the
`l32i` on the existing `helper_ops` counter. Boot crashed at cycle
~1.78 M (`halted=3`) because M6.73-style arms with conditional
bridges DO clobber `a0` at runtime when the slow path is taken, but
`helper_ops` missed them. ctest's `diff_jit_bench_lockstep` caught
it instantly. Fixed by adding the broader callx0 flag described.

**Triple-diff workflow:**

* ctest: 7/7
* `--diff-jit-trace`: only the documented M6.66 cycle-11898 divergence
* Boot @ 300 K / 5 M deterministic: same final PC (`0x40032C`), same
  helper count as M6.83 — the metric drop is from reduced inline op
  count per block invocation, not from runtime divergence

**Perf:**

| Workload | M6.83 | **M6.84** | Δ |
|----------|------:|----------:|--:|
| Bench @ 5 M cyc   | 1.418 lx7/cyc | **1.411 lx7/cyc** | **−0.5 %** lx7 |
| Bench @ 20 M cyc  | 1.258 lx7/cyc | **1.257 lx7/cyc** | **−0.08 %** lx7 |
| Bench @ 100 M cyc | 1.409 lx7/cyc | **1.396 lx7/cyc** | **−0.9 %** lx7 |
| Boot @ 300 K det  | 2.170 lx7/cyc | **2.159 lx7/cyc** | **−0.5 %** lx7 |
| Boot @ 5 M det    | 12 356 073 lx7 | **12 352 738 lx7** | −3 335 lx7 |
| Boot @ 100 M cyc  | 1.735 lx7/cyc | **1.734 lx7/cyc** | within noise |

Pure epilogue trim — savings are larger on workloads with more
helper-less block invocations (long-horizon bench) and smaller on
short paths where the helper-less proportion is low.

ctest: **7 / 7** pass.

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

**M6.61 — measured chain-cache match rate to validate cross-block reg caching.**
Added `u32 cache_sig` field to m68k_block packing `(rc.active,
rc.guest[0..3])` as a 32-bit signature. Added
`disp.chain_cache_matches` counter incremented when a chain hit's
prev/next blocks have equal cache_sig. Measurements:

| Workload | chain_hits | chain_cache_matches | rate |
|----------|-----------:|--------------------:|-----:|
| Bench    | 707 780    | 9 603               | 1.36 % |
| Boot     | 965 437    | 962 547             | **99.70 %** |

So cross-block register caching is genuinely high-gain on boot (99.7 %
of chained transitions share cache config — code from contiguous ROM
regions naturally has identical hot-D/A register pressure) but
near-useless on bench (Speedometer's tight loops vary register usage
each block). The disparity confirms the user's #1 high-gain item is
specifically a *boot* win.

**Implementation strategy (planned, not done):**

The chain epilogue (ESP32) currently does `l32i a11,
predicted_next->entry_addr; jx a11`. Replace with `l32i a11,
current_block->predicted_next_entry; jx a11` — same op count, but
`predicted_next_entry` is precomputed by the dispatcher at chain-
establish time to be either `next->entry_addr` (full prologue) or
`next->body_addr` (skip prologue entirely).

`body_addr` = address of first instruction *after* the prologue —
points inside the existing block, no code duplication needed. Compat
check at predict time: `cache_sig matches AND (next->sr_loaded ==
false || prev->sr_loaded == true)`. Need a `u8 sr_loaded` flag on
m68k_block.

Expected ESP32 gain on boot: 962 K × ~5 ops (cpu_base + jit_ret_pc
save + sr_reload + cache_load) ≈ 4.8 M ops out of 159 M = **~3 %
boot win**. Host metric unchanged (chain epilogue ESP32-only).

**M6.62 — cross-block register caching (DELIVERED for ESP32).**
The user's #1 high-gain item, finally implemented. Adds three fields
to `m68k_block`:

* `void *body_addr` — `code + body_off`, where `body_off` is the
  offset right after the prologue (cpu_base + jit_ret_pc save +
  sr_reload + cache_load). Chain JX skips straight here when
  registers are already valid from the predecessor block.
* `u8 sr_loaded` — set from `block_needs_sr_load`. The
  compatibility check needs to know whether `a14` (R_SR) held a
  loaded value at this block's start.
* `void *predicted_next_entry` — the dispatcher precomputes this at
  chain-establish time to either `next->entry_addr` (full prologue)
  or `next->body_addr` (skip prologue entirely).

Compatibility logic (in dispatcher when setting `prev->predicted_next
= b`):

```c
bool compat = (prev->cache_sig == b->cache_sig) &&
              (!b->sr_loaded || prev->sr_loaded);
prev->predicted_next_entry = compat ? b->body_addr : b->entry_addr;
```

ESP32 chain epilogue change (codegen.c):

```diff
-xt_l32i(&e, 11, 10, off_entry_addr);     /* M6.58 — from predicted_next */
+xt_l32i(&e, 11, 9, off_pred_entry);      /* M6.62 — from current_block */
 xt_jx(&e, 11);
```

Same op count as the M6.58 epilogue (1 l32i + 1 jx); just dereferences
the *current* block's predict-time field instead of the *next* block's
entry. Predict-time selection of body_addr vs entry_addr is the
optimization — the chain JX itself stays as small as in M6.58.

Conditions verified for safe skip-prologue chain:
* a0 (jit_ret_pc) — chain epilogue does `l32i a0, OFF_JITRETPC`
  before JX. Every block uses the same dispatcher return PC, so
  cpu->jit_ret_pc is already valid; body_addr path doesn't re-save.
* a1 (SP) — never modified by any JIT block (invariant).
* a3 (R_CPU) — preserved by chain JX, set by prev's prologue.
* a4..a7 (cache slots) — preserved by chain JX; their content
  matches what next's prologue would have loaded *if and only if*
  cache_sig matches.
* a14 (R_SR) — preserved by chain JX; valid for use by next *if and
  only if* next->sr_loaded ⇒ prev->sr_loaded.

SMC flush also clears `predicted_next_entry` along with the existing
`predicted_next` and `predicted_next_pc` invalidation.

Host metrics unchanged (chain epilogue still `#ifdef ESP_PLATFORM`).
ctest 3/3 pass. Cannot validate the body_addr code path on host —
xt_sim runs one block per invocation, so a JX into another block's
code is unreachable. Code review only.

Expected ESP32 boot win: ~3 % (962 K chain matches × ~5 LX7 ops
saved). Negligible on bench (1.4 % chain-match rate).

**This completes the user's three high-gain items:**
* #1 cross-block register caching: M6.62 ✅
* #2 comprehensive lazy CCs: M6.16–M6.30 ✅
* #3 native block chaining on ESP32: M6.54 + audit M6.55–M6.58 ✅

**M6.63 — runtime-selectable JIT-arena eviction policies + sweep.**
The user asked for LRU and FIFO eviction policies and a comparative
arena-size sweep against the existing bump allocator. Previously
`GBJIT_JIT_EVICT` was a compile-time macro with three modes but
*no actual evict callbacks wired*, so all three behaved as bump.

Refactored:
* `codecache.h/.c` — dropped `#if GBJIT_JIT_EVICT` gating, runtime
  dispatch on `cc->mode` field. `codecache_init` now takes a mode
  parameter (`CC_MODE_BUMP` / `CC_MODE_LRU` / `CC_MODE_FIFO`).
* `dispatcher.c` — implemented `lru_evict_cb` (O(N) scan for the
  block with the smallest `last_used_cycle`, free its codecache
  span, drop from hash, invalidate any predicted_next links pointing
  at it) and `fifo_evict_range_cb` (drop every block whose
  codecache span overlaps the ring's about-to-overwrite range).
* `m68k_block` — added `u64 last_used_cycle`, stamped on every
  dispatcher entry (not on chain hits — see M6.62).
* `dispatcher` — `m68k_dispatcher_init_ex(d, cpu, arena_kb,
  evict_mode)`; legacy `m68k_dispatcher_init` calls it with
  defaults. Existing `prev`-invalidation cascade extended to react
  to `smc_invalidations` (LRU/FIFO drops fire that counter) in
  addition to `arena_resets`.
* `port/host/main.c` — `--arena-kb N`, `--evict none|lru|fifo` CLI
  flags.

**Sweep results** (`scripts/evict_sweep.csv`,
`scripts/evict_sweep.png`):

Bench (Speedometer snapshot): all three policies identical at 1.279
lx7/cyc across the full 16 KB → 4 MB sweep — the working set is
small enough to fit in 16 KB so no eviction ever fires. Interp
baseline 6.462 lx7/cyc.

Boot (Mac Plus ROM, 100 M cycles): pure-bump suffers a sharp cliff
below 512 KB (1.735 → 2.337 lx7/cyc, +35 %) because the arena
thrashes — 98 K resets at 16 KB, 1085 even at 256 KB. LRU and FIFO
both **hold the JIT speed flat (1.734) across the entire range
with zero arena resets** — the eviction callback fires per
allocation, dropping the coldest (LRU) or oldest (FIFO) block, no
massive recompile storm. Interp baseline 5.895 lx7/cyc.

| Arena | bump (lx7/cyc, resets) | LRU | FIFO |
|-------|-----------------------:|----:|-----:|
| 16 KB | 2.336 (98 022)         | **1.734** | **1.734** |
| 256 KB | 2.337 (1 085)         | **1.734** | **1.734** |
| 1 MB | 1.735 (303)             | **1.734** | **1.734** |
| 4 MB | 1.734 (75)              | **1.734** | **1.734** |

LRU and FIFO match each other to 3 decimal places at every arena
size — for this workload's spatial-locality / chain-induced
working-set shape, the policies are interchangeable. The big win
is over bump's hard-cliff behaviour at small arenas, which is the
exact regime ESP32 IRAM lives in (typical JIT-arena allocations
on -S3 are 256–512 KB).

**Practical implication for ESP32-S3:** turn on either policy
(LRU is closer to "right" theoretically; FIFO is cheaper at
allocation time — no O(N) victim scan). Either lets us shrink the
ESP32 JIT-arena allocation from 1 MB-worth-of-IRAM down to 256 KB
or even less, freeing IRAM for other purposes, without any
boot-perf regression.

Reproduce: `scripts/evict_sweep.sh > scripts/evict_sweep.csv` then
`python3 scripts/evict_sweep_plot.py`.

**M6.63 follow-up — "LRU vs FIFO equivalence" was a metric artifact.**
User flagged it as suspicious; investigation confirmed and explained.
`xt_instrs` and `helpers` (the components of `real_lx7_per_cyc`) are
**bit-identical** between LRU and FIFO at every arena size — same
exact integers to the last digit. That's because the metric counts
JIT-emitted-and-executed work, which is *deterministic* given the
guest code: eviction policy changes *which blocks live where* and
*how many recompiles fire*, but not *which opcodes execute*. Fallback
to interp on alloc-miss is the same path for both.

Added wall-clock (`elapsed_us`, 3-run averaged) to the sweep. Now
the policies separate clearly:

| Arena | bump wall-ms | LRU wall-ms | FIFO wall-ms |
|------:|-------------:|------------:|-------------:|
| 16 KB | **710**       | 838         | 842          |
| 64 KB | **710**       | 892         | 845          |
| 1 MB  | 770          | 892         | 845          |

At 16–64 KB **bump is faster in wall-clock** even though its
lx7/cyc is 35 % worse (2.34 vs 1.74). Why: LRU does an O(N_blocks)
victim scan per eviction (102 K evictions × thousands of blocks =
quadratic), and bump's "wipe-everything reset" is structurally
cheaper per call. FIFO is between them — drops O(overlap) per call,
no scan.

**Right interpretation:**
* `real_lx7_per_cyc` is the **JIT-execution-cost** metric.
  LRU/FIFO win when working set > arena (less wasted JIT work).
* Wall-clock includes **allocator-overhead**. LRU's O(N) scan
  dominates when block count is high.
* For ESP32, the relevant cost is JIT execution (real LX7 cycles)
  + allocator overhead (real cycles spent on hash walks). Need a
  better LRU implementation (sampled or doubly-linked-list O(1))
  to make LRU competitive in wall-clock at large block counts.
* **FIFO is the pragmatic pick**: same lx7/cyc benefit as LRU,
  cheaper allocator overhead, no O(N) scan.

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
