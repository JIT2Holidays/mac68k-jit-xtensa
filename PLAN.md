# mac68k-jit-xtensa — Design

## 1. Goal

Run a Motorola 68000 Macintosh on an ESP32-S3 (dual-core Xtensa LX7 @
240 MHz). The 68000 is roughly two orders of magnitude slower per-clock
than the LX7, but a plain interpreter still loses too much to fetch/
decode/dispatch overhead. So the CPU is **JIT-compiled**: 68000 basic
blocks are translated, once, into native Xtensa machine code that the
LX7 then executes directly.

`gbjit-xtensa` (a Game Boy JIT for the same target) is the reference for
the infrastructure — its Xtensa LX7 encoder, code cache, host Xtensa
simulator and CALL0⇄windowed bridge are reused here essentially verbatim.
mini vMac is the reference for the Macintosh peripherals.

## 2. The Xtensa target — the things that bite

- ESP32-S3 uses the **CALL0 ABI** internally for the JIT (no register
  windows); IDF's C code is **windowed**. Crossing that boundary needs a
  hand-written trampoline that never touches `a1`.
- The code cache must be **executable IRAM**, allocated `MALLOC_CAP_EXEC`.
  IRAM rejects 8-bit stores (`LoadStoreError`) — all code emission and
  buffer clearing is done with 32-bit word stores.
- 24-bit instructions; a 16-bit "density" subset exists but is unused.
- `L32R` loads a 32-bit literal from a pool placed *before* the code.

## 3. The 68000 — what makes it harder than the GB CPU

- 16 × 32-bit registers (D0-D7, A0-A7), a 16-bit SR, big-endian memory.
- **Variable-length instructions**: a 16-bit opcode word plus 0-N
  extension words. The JIT's block walker must size every instruction.
- 12 effective-addressing modes, several with their own extension words
  and pre-decrement / post-increment side effects.
- Five condition codes (X N Z V C) with per-instruction-class rules.

## 4. JIT design

### 4.1 Translation unit
A basic block starts at a PC and runs forward until a control-flow
instruction (branch / jump / return / trap / stop), capped at 64
instructions. `m68k_decode_at` sizes each instruction without executing
it; this MUST agree with the interpreter's actual decode or the runtime
PC desyncs from the discovered block (a real bug found and fixed during
bring-up — see STATUS.md).

### 4.2 Block memory layout
```
+------------------+ <- block->code
|  literal pool    |   cpu_state base + helper address
+------------------+ <- entry_off
|  prologue        |   a3 = cpu_state base; stash CALL0 return PC
|  body            |   per-instruction: inline native, or CALLX0 helper
|  epilogue        |   restore return PC; JX back to the dispatcher
+------------------+
```

### 4.3 Register convention (CALL0)
`a0` return address (stashed in `cpu->jit_ret_pc`, clobbered by CALLX0),
`a1` stack pointer (never touched), `a2` call argument, **`a3` the
cpu_state base — it survives helper calls** (the trampoline preserves
the windowed-callee-safe `a0..a7` range), `a8..a15` inline scratch.

### 4.4 Inline vs fallback
The JIT emits native Xtensa for ~30 hot opcodes / patterns — every
MOVE / MOVEQ / MOVEA variant, the immediate and register ADD / ADDQ /
ADDA / SUB / SUBQ / SUBA / CMP / CMPI / CMPM forms, NEG / TST / CLR,
ADDX / SUBX / NEGX, the bit ops, JSR / JMP / RTS (with in-RAM fast
paths that read 4 BE bytes inline instead of bridging), Bcc.S / DBcc,
fused-CMP+Bcc terminators, MOVE.L push / pop patterns, MOVEM predec /
postinc inlined under a size threshold, and BTST / ORI MMIO fast
helpers — including full X N Z V C computation derived from the
standard carry / overflow bit identities. Lazy-CC liveness avoids
materialising flags that no downstream op reads. Every other
instruction becomes a `CALLX0` into the reference interpreter's
`m68k_step` (or, for hot patterns where m68k_step's decode dominates,
a custom fast-helper that takes the operands via `cpu->jit_arg1/2`).

### 4.5 Dispatch & chaining
The dispatcher hashes blocks by start PC and keeps a per-block
"predicted next" cache so a hot block re-enters its successor without
a hash probe. **On the ESP32 build**, the block epilogue ends with a
`jx` directly to the predicted successor's entry — the dispatcher is
bypassed entirely when prediction hits, bounded by a `chain_budget`
counter that forces an occasional dispatcher return so VIA / IRQs get
serviced (~16 chained blocks max; sub-ms IRQ latency).

When the predecessor's register-cache configuration matches the
successor's, the chain JX skips the next block's prologue too — going
straight to the body, R_SR + cache slots already valid in registers
(M6.62; 99.7 % of boot's chain hits qualify). On the host build the
chain epilogue is `#ifdef ESP_PLATFORM`-gated — the in-tree Xtensa
simulator runs one block per invocation and can't follow a JX out of
the current block's code memory.

### 4.6 Codecache + eviction
A single-arena codecache holds compiled blocks. Three eviction policies
are selectable at runtime via `--evict`: bump (default, hard cliff on
arena overflow), per-block LRU (free-list with O(N) coldest-block
scan), and circular FIFO (ring write cursor evicts overlapping blocks).
A sweep at multiple arena sizes (`scripts/evict_sweep.sh`) shows bump
suffers a 35 % cliff under 512 KB on boot while LRU and FIFO hold flat
across 16 KB → 4 MB. Practical ESP32 implication: the JIT-arena IRAM
allocation can shrink to 256 KB with FIFO eviction without regression.

### 4.7 Host vs target
The same `jit/` code serves both. On the host (x86 / arm64) generated
blocks cannot run natively, so they execute on `jit/xtensa_sim.c` —
a small, independently-written Xtensa interpreter. This lets `ctest`
actually *run* JIT output on every host build. On the ESP32-S3 the
code runs directly on the LX7.

### 4.8 Correctness workflow
`ctest` runs four checks: the interpreter / encoder unit tests, a
JIT/interp differential on hand-crafted snippets, and a lockstep
block-by-block diff (`--diff-jit-trace`) on a frozen Speedometer
snapshot. The lockstep test is what catches "the JIT inline arm for
op X drifted from the interpreter" — see M6.68 in `STATUS.md` for a
walk-through of a real SR-flush bug it found that the snippet suite
had missed.

## 5. Macintosh model

Memory map (24-bit bus): RAM at 0 (ROM overlaid until cleared), ROM at
0x400000, a 6522 VIA in the 0xE8xxxx region, and harness debug ports at
0xF00000 (serial out + a clean guest-exit port). The VIA models the T1
timer and the ~60 Hz vertical-blank interrupt (autovectored to IPL 1).
A 512×342 1bpp framebuffer is carved from the top of RAM.

## 6. Milestones

1. **M1 — interpreter + memory** ✅ reference 68000 core, Mac memory /
   VIA, the demo ROM. Self-tested.
2. **M2 — Xtensa encoder + sim** ✅ reused from gbjit; round-trip tested.
3. **M3 — basic JIT (host)** ✅ block discovery, inline fast set +
   interp fallback, dispatcher, predicted-next chaining.
   Differential-tested against the interpreter.
4. **M4 — JIT on ESP32-S3 under QEMU** ✅ executable IRAM arena, the
   CALL0 ⇄ windowed bridge, native block execution. `RESULT: PASS`.
5. **M5 — first optimisation pass** ✅ reverted after a `--diff-jit`-
   gated correctness check (added afterwards) found bugs the demo-only
   ctest had missed. Tooling kept: `--load-snapshot`, `--profile`,
   `--diff-jit`, the xtensa_sim instruction counter, the M5 ALU
   snippets, the test_jit branch. Subsequent passes do each
   optimisation behind that gate one at a time.
6. **M6 — real Mac ROM** ✅ boots a real Macintosh Plus ROM with
   System 6.0.8 to the Finder desktop. Needed: the Mac Plus
   peripheral model (VIA / RTC / SCC / SCSI), CPU bug fixes
   (line-A traps, MOVEP, ABCD / SBCD / NBCD), mini vMac's `.Sony`
   driver patch for floppy I/O, and JIT self-modifying-code
   invalidation.

   The post-M6 work (M6.10 → M6.70 in `STATUS.md`) is the bulk of the
   JIT's perf and correctness state today:

   - **M6.10–M6.30** — within-block register caching, comprehensive
     lazy CC liveness, fused CMP+Bcc terminators, ADDX2-based cycle
     fuse, sext memoization. Bench moved from ~6 lx7/cyc to 1.279
     (5.05 × interp, exceeding the original 5 × goal).
   - **M6.30–M6.50** — inlined RTS, JMP (xxx).L, MOVE.L push / pop
     extended to An sources, MOVEM small-N inlines, fast-path MMIO
     helpers (ORI.B, BTST, MOVEM.L (An)+, MOVE.W (An)+,Dn). Cycle-
     accounting bugs fixed (M6.52: ADD.W cycle, M6.53: ADDA.W cycle).
   - **M6.54** — native block chaining on ESP32 with `chain_budget`
     and `current_block` cpu-state fields. Host metrics unchanged
     (chain epilogue is `#ifdef ESP_PLATFORM`-gated); estimated
     ~12 % boot win on hardware.
   - **M6.55–M6.58** — chain-correctness audit: a0 / jit_ret_pc
     restored before chain JX (M6.55), `chain_budget = 0` on SMC
     writes (M6.56), `chain_budget = 0` on STOP / RESET / halt
     (M6.57), pre-computed `entry_addr` per block (M6.58, −2 LX7
     ops per chain hit).
   - **M6.61–M6.62** — cross-block register caching. Measurement
     showed 99.7 % of boot's chain transitions share cache layout;
     the chain JX target is precomputed at predict time to either
     `entry_addr` (full prologue) or `body_addr` (skip prologue
     entirely, registers already valid). ~3 % additional boot win
     on hardware.
   - **M6.63** — runtime-selectable code-cache eviction (bump / LRU /
     FIFO) plus a per-arena-size sweep (`scripts/evict_sweep.sh` →
     `scripts/evict_sweep.png`). Confirmed FIFO is the pragmatic
     ESP32 pick — same `lx7_per_cyc` as LRU at fraction of
     allocator overhead.
   - **M6.64–M6.67** — scripted-run keyboard support, MacBench /
     THINK C bench-target notes, full boot-script coordinate
     mapping (Infinite HD opens at correct icon centre).
   - **M6.68–M6.70** — SR-flush latent bug class found via
     `--diff-jit` (lurking since at least M6.61, missed by ctest's
     curated snippets): `emit_helper_step_after_flush_undo` and
     `emit_jit_fast_helper` cleared `g_sr_dirty` at compile time
     even though their `emit_sr_flush` was inside a runtime
     slow-path branch. Fix: snapshot + restore. New ctest
     (`diff_jit_bench_lockstep`) locks the fix in.

   Triple-differential (JIT vs interp vs mini vMac) is now SOP after
   any JIT-affecting change. See `STATUS.md` for the full progression
   and `memory/triple-differential.md` for the workflow rationale.

## 7. What's left

Real-hardware deployment is the dominant remaining work. The M6.54
chaining + M6.62 cross-block reg cache + M6.63 eviction-policy choice
are all coded and host-tested but only code-reviewed on ESP32 —
because the in-tree Xtensa simulator can't follow a JX out of the
current block's code memory. Measuring on real ESP32-S3 hardware is
the next concrete step.

Smaller plate items, in rough decreasing order of impact:
- Trace-based / superblock JIT (combine multiple blocks).
- More inlined hot opcodes (incremental, ~0.1-0.5 % per add).
- Inline simple helper bodies (avoid CALLX0 overhead).
- Address-error trap on odd PC (could short-circuit a ~6 % boot
  excursion through unmapped memory, with behavioural risk).
- Per-instruction `mac_mem_tick` in JIT to align VIA timing with
  the interpreter (would extend `--diff-jit` reach past ~12 K
  cycles but at significant perf cost).
- MacBench / THINK C bench snapshots for broader differential
  coverage.
