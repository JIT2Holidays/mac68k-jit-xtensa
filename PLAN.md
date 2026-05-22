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
The JIT emits native Xtensa for a curated fast set (MOVE/MOVEQ/MOVEA,
ADD/ADDQ/SUBQ, CMP, NOP) — including full X/N/Z/V/C computation derived
from the standard carry/overflow bit identities. Every other instruction
becomes a `CALLX0` into the reference interpreter's `m68k_step`. The
fallback makes the JIT correct by construction; the differential test
exists to catch the inline paths drifting from the interpreter oracle.

### 4.5 Dispatch & chaining
The dispatcher hashes blocks by start PC and keeps a single-slot
"predicted next" cache so a hot block re-enters its successor without a
hash probe. When the code-cache arena fills it is wiped wholesale and
re-warmed lazily.

### 4.6 Host vs target
The same `jit/` code serves both. On the host (x86) generated blocks
cannot run natively, so they execute on `jit/xtensa_sim.c` — a small,
independently-written Xtensa interpreter. This lets `ctest` actually
*run* JIT output on every host build. On the ESP32-S3 the code runs
directly on the LX7.

## 5. Macintosh model

Memory map (24-bit bus): RAM at 0 (ROM overlaid until cleared), ROM at
0x400000, a 6522 VIA in the 0xE8xxxx region, and harness debug ports at
0xF00000 (serial out + a clean guest-exit port). The VIA models the T1
timer and the ~60 Hz vertical-blank interrupt (autovectored to IPL 1).
A 512×342 1bpp framebuffer is carved from the top of RAM.

## 6. Milestones

1. **M1 — interpreter + memory** ✅ reference 68000 core, Mac memory/VIA,
   the demo ROM. Self-tested.
2. **M2 — Xtensa encoder + sim** ✅ reused from gbjit; round-trip tested.
3. **M3 — basic JIT (host)** ✅ block discovery, inline fast set + interp
   fallback, dispatcher, predicted-next chaining. Differential-tested
   against the interpreter.
4. **M4 — JIT on ESP32-S3 under QEMU** ✅ executable IRAM arena, the
   CALL0⇄windowed bridge, native block execution. `RESULT: PASS`.
5. **M5 — optimisation** (future) the basic block JIT pays a full
   dispatch round-trip per loop iteration, so on tight loops it does not
   yet beat the interpreter. The next step is native block chaining and
   back-edge internalisation (keep a loop inside one block with an
   internal Xtensa branch), then a wider inline opcode set and inline
   memory fast-paths. See STATUS.md.
6. **M6 — real Mac ROM** ✅ boots a real Macintosh Plus ROM with System
   6.0.8 to the Finder desktop. Needed: the Mac Plus peripheral model
   (VIA/RTC/SCC/SCSI), two CPU bug fixes (line-A traps, MOVEP), mini
   vMac's `.Sony` driver patch for floppy I/O, and JIT self-modifying-
   code invalidation. See STATUS.md.
