# 68000 Instruction Coverage in the JIT

Reflects coverage through **M6.204**. Bulk-tabular state was captured
at M6.161; subsequent inline series (M6.169-M6.193 thinkc8 hotspots,
M6.196 fusion, M6.198-M6.204 boot tuning) are summarized in the
"M6.169+ delta" section at the bottom rather than re-flowed through
every row. This document inventories every 68000 instruction class
the JIT recognises in `jit/codegen.c` and the translation strategy
used. The Mac Plus is plain MC68000 — no 68010/020+ extensions
(BFEXTU, MOVES, divs.l etc.) — so the table below is the complete
ISA target.

> If you're hunting for a hot helper to inline, **search this file first**:
> the "m68k_step" rows are the remaining opportunities. The "inline-pure"
> ones are immune to the M6.66 trajectory trap (see
> `memory/bridge-only-arms-trajectory-shift.md`).

## Translation styles

| Style | Marker | What it does |
|-------|:------:|--------------|
| **Inline pure** | ✅ | Translated entirely to native Xtensa LX7 ops; no CALLX0. Safe to extend regardless of boot-100M trajectory. |
| **Inline + fast helper** | ⚡ | Bounds-checked RAM/ROM fast path emits native code; slow path bridges to a small custom C helper (`m68k_jit_*`) that skips `m68k_step`'s decode + `cpu->instrs++`. |
| **Inline + m68k_step bridge** | 🪝 | Bounds-checked RAM fast path; slow path falls to `m68k_step` via `emit_helper_step_after_flush_undo`. |
| **Fusion-only** | 🔗 | Recognised only as part of a multi-op fusion (TST+Bcc, MOVE+Bcc, LEA+ADDA). Standalone form is `m68k_step`. |
| **Compile-time analysis** | 🧠 | Affects code emission but emits no per-op runtime code (cross-block successor, helper-touched mask narrowing). |
| **m68k_step fallback** | 🐢 | Not recognised by the JIT; the interpreter's `m68k_step` runs the op. 64 LX7 / call in the host cost model. |

## Coverage at a glance

| Group | Inline pure | Inline + helper | Fusion / Analysis | m68k_step |
|-------|:-----------:|:---------------:|:-----------------:|:---------:|
| Data movement | 16 | 22 | 4 | 1 |
| Arithmetic | 18 | 7 | 0 | 3 |
| Logical | 8 | 4 | 0 | 1 |
| Bit ops | 8 | 4 | 0 | 4 |
| Shift / rotate | 9 | 0 | 0 | 9 |
| Compare | 5 | 4 | 0 | 4 |
| Control flow | 11 | 3 | 1 | 4 |
| System | 1 | 2 | 0 | 10 |
| **Totals** | **76** | **46** | **5** | **36** |

The bare numbers undersell the inline coverage on hot paths. The ~21K-fire
opcodes that drive bench's `lx7/cyc` metric are all inline; the remaining
m68k_step rows are mostly rare-fire system / exception ops.

---

## Data movement

| Mnemonic | Sizes | Mode coverage | Style | Milestone | Notes |
|----------|:-----:|---------------|:-----:|----------:|-------|
| `MOVE Dm,Dn` | .B / .W / .L | Dn → Dn | ✅ | M5.7 / M6.88 / M6.101 | M6.88: flags-dead lean 4-op path |
| `MOVE Am,Dn` | .L | An → Dn | ✅ | M5.7 | |
| `MOVE Dn,(An)` | .B / .W / .L | Dn → (An) | ⚡ | M6.92 / M6.93 / M6.125 | byte fast helper; .W flag-only |
| `MOVE Dn,(An)+` | .B / .L | Dn → (An)+ | ⚡ | M6.94 / M6.133 | `m68k_jit_move_l_dn_to_anpi_mmio` |
| `MOVE Dn,-(An)` | .L / .W | Dn → -(An) | ⚡ | M6.93 / M6.125 | |
| `MOVE Dn,(d16,An)` | .W / .B | Dn → (d16,An) | ⚡ | M6.118 / M6.135 | byte: `move_b_imm_to_addr_mmio` shared |
| `MOVE Dn,(xxx).W` | .B / .W | Dn → abs.W | ⚡ | M6.110 | |
| `MOVE (An),Dn` | .B / .W / .L | (An) → Dn | ⚡ | M6.92 / M6.101 / M6.127 | M6.144 swaps slow path to `move_l_an_to_dn_mmio` |
| `MOVE (An)+,Dn` | .B / .L / .W | (An)+ → Dn | ⚡ | M6.94 / M6.127 / M6.132 | `m68k_jit_move_l_postinc_to_dn_mmio` |
| `MOVE (d16,An),Dn` | .B / .L | (d16,An) → Dn | ⚡ | M6.91 / M6.199 | |
| `MOVE (d8,An,Xn),Dn` | .W / .L | indexed → Dn | ⚡ | M5.5 | |
| `MOVE (xxx).W,Dn` | .B / .W / .L | abs.W → Dn | ⚡ | M6.108 / M6.109 | bench-hot 0x2438 |
| `MOVE (xxx).L,Dn` | — | — | 🐢 | — | falls to m68k_step |
| `MOVE (An),(An)` | mem-to-mem | M5.8 cluster | ⚡ | M5.8 | |
| `MOVE (An)+,(Am)+` | .L | post→post mem-mem | ⚡ | M6.130 | ROM-source fast read (M6.76) |
| `MOVE (d16,An),(Am)` | .B | mem-to-mem | ⚡ | M6.91 / M6.134 | bench-hot inner loop |
| `MOVE #imm,(d16,An)` | .B | imm → (d16,An) | ⚡ | M6.135 | `move_b_imm_to_addr_mmio` |
| `MOVE #imm,SR` | — | imm.S=1 fast | ⚡ | M6.117 | privilege-check + bridge |
| `MOVE SR,Dn` | — | — | 🐢 | — | privileged on 68000? (no — user-readable) |
| `MOVE to CCR` | — | — | 🐢 | — | falls to m68k_step |
| `MOVEA Am,An` | .L | An → An | ✅ | M5.3 | |
| `MOVEA Dm,An` | .L | Dn → An | ✅ | M5.9 | aggressive — delayed-bug-cleared |
| `MOVEA (An)+,Am` | .L | (An)+ → Am | ⚡ | M6.103 / M6.189 | |
| `MOVEA (An),Am` | .L | (An) → Am | ⚡ | M6.103 | |
| `MOVEA (xxx).W,Am` | .L | abs.W → Am | ⚡ | M6.108 | sibling of MOVE.L (xxx).W,Dn |
| `MOVEQ #imm,Dn` | .L | imm → Dn | ✅ | M5.7 | |
| `MOVEM.L (An)+,reg` | .L | postinc → regs | ⚡ | M6.45 | `m68k_jit_movem_l_postinc_to_regs` |
| `MOVEM.L reg,-(An)` | .L | regs → predec | ⚡ | M6.45 | `m68k_jit_movem_l_predec_from_regs` |
| `MOVEM.W reg,(An)` | .W | regs → mem | ⚡ | M6.45 | |
| `MOVEM.L reg,(An)` | .L | regs → mem | ⚡ | M6.45 | |
| `MOVEM other forms` | — | — | 🐢 | — | (d16,An) / abs forms |
| `MOVEP` | — | — | 🐢 | — | unused on Mac Plus, slow path is fine |
| `LEA (d16,An),Am` | — | — | ✅ | M5.5 | |
| `LEA (d8,An,Xn),Am` | — | indexed | ✅ | M5.5 | M6.85b: fusion with following ADDA.W |
| `LEA (d16,PC),An` | — | PC-relative | ✅ | M6.107 | bench-hot 0x41FA |
| `LEA (xxx).W / .L` | — | abs | ✅ | M5.5 | |
| `PEA` | — | all modes | 🐢 | — | hot at bench 0x486E (47 fires / 100M) |
| `LINK` | — | — | 🐢 | — | hot at bench 0x4E56 (40 fires) |
| `UNLK` | — | — | 🐢 | — | hot at bench 0x4E5E (34 fires) |
| `EXG` | — | — | 🐢 | — | rare |
| `SWAP Dn` | — | low / high swap | ✅ | M5.7 | |

## Arithmetic

| Mnemonic | Sizes | Mode coverage | Style | Milestone | Notes |
|----------|:-----:|---------------|:-----:|----------:|-------|
| `ADD Dm,Dn` | .B / .W / .L | reg-reg | ✅ | M5.1 / M6.112 | |
| `ADD (An),Dn` | .W / .L | mem→reg | 🪝 | M5.8 | bridge |
| `ADD Dn,(An)` etc. | — | reg→mem | 🐢 | — | partial; some bench-hot variants reverted |
| `ADDA.W Dn/An,Am` | .W | reg → Am | ✅ | M6.104 | |
| `ADDA.W #imm,An` | .W | imm.W → An | ✅ | M5.3 | bench-hot 0xD0FC |
| `ADDA.L Dn/An,Am` | .L | reg → Am | ✅ | M6.152 | sibling of M6.104 |
| `ADDI #imm,EA` | .B / .W / .L | imm + EA | ✅ | M5.2 | |
| `ADDQ.B #imm,Dn` | .B | imm + Dn | ✅ | M6.160 | shift-to-high-8 trick; lean flags-dead path |
| `ADDQ.W #imm,Dn` | .W | imm + Dn | ✅ | M5.6 | |
| `ADDQ.L #imm,Dn` | .L | imm + Dn | ✅ | M5.2 | |
| `ADDX.L Dm,Dn` | .L | reg-reg | ✅ | M6.142 | X-flag consumer |
| `SUB Dm,Dn` | .B / .W / .L | reg-reg | ✅ | M5.1 / M6.112 | |
| `SUB (An),Dn` etc. | .W / .L | bridge | 🪝 | M5.8 | |
| `SUBA.W #imm,An` | .W | imm.W → An | ✅ | M5.3 | |
| `SUBA.L Dn/Am,An` | .L | reg → An | ✅ | M6.157 | sibling of M6.152 ADDA.L |
| `SUBI #imm,EA` | .B / .W / .L | | ✅ | M5.2 | |
| `SUBQ.B #imm,Dn` | .B | | ✅ | M6.160 | bench 0x5205, boot 0x5801 |
| `SUBQ.W #imm,Dn` | .W | | ✅ | M5.6 | |
| `SUBQ.L #imm,Dn` | .L | | ✅ | M5.2 | |
| `SUBX.L Dm,Dn` | .L | reg-reg | ✅ | M6.142 | X-flag consumer |
| `MULU.W Dm,Dn` | .W → .L | reg-reg | ✅ | M6.159 | sibling of M6.81's #imm16 and (d16,An) arms |
| `MULS.W Dm,Dn` | .W → .L | reg-reg | ✅ | M6.159 | |
| `MULU.W / MULS.W #imm16,Dn` | .W → .L | imm | ✅ | M6.81 | |
| `MULU.W / MULS.W (d16,An),Dn` | .W → .L | (d16,An) | ✅ | M6.81 | |
| `DIVU.W / DIVS.W` | — | — | 🐢 | — | exception semantics (zero-div) — slow path is safe |
| `NEG Dn` | .B / .W / .L | reg | ✅ | early | |
| `NEGX.L Dn` | .L | reg | ✅ | M6.115 | X-flag consumer (X-form fix) |
| `EXT.W Dn` | .W | Dn[7:0] → Dn[15:0] | ✅ | M6.99 | |
| `EXT.L Dn` | .L | Dn[15:0] → Dn[31:0] | ✅ | M6.99 | boot-warm 0x48C1 |
| `ABCD / SBCD / NBCD` | .B | reg-reg | ✅ | cd1fd7a | per-byte BCD + X handling |

## Logical

| Mnemonic | Sizes | Mode coverage | Style | Milestone | Notes |
|----------|:-----:|---------------|:-----:|----------:|-------|
| `AND Dm,Dn` | .B / .W | reg-reg | ✅ | M6.100 | |
| `AND.L Dm,Dn` | .L | reg-reg | ✅ | M5.1 | |
| `AND (xxx).W,Dn` | .L | abs.W → Dn | ⚡ | M6.111 | |
| `OR Dm,Dn` | .B / .W / .L | reg-reg | ✅ | M5.1 / M6.100 | |
| `OR (xxx).W,Dn` | .L | abs.W → Dn | ⚡ | M6.111 | |
| `EOR Dn,Dm` | .B / .W / .L | reg-reg | ✅ | M5.1 / M6.100 | |
| `EOR Dn,(An)` etc. | — | reg→mem | 🐢 | — | M6.145 attempt reverted |
| `NOT Dn` | .B / .W / .L | reg | ✅ | M6.156 | bench 0x4641 NOT.W D1 (36 fires); sibling of M6.139 CLR |
| `ANDI/ORI/EORI #imm,Dn` | .B/.W/.L | imm → Dn | ✅ | M5.2 | |
| `ANDI/ORI/EORI to-CCR/SR` | — | — | 🐢 | — | privileged for SR; CCR rare |
| `ORI.B #imm,(d16,An)` | .B | imm → MMIO | ⚡ | M6.31 | `m68k_jit_ori_b_mmio` |
| `CLR Dn` | .B / .W / .L | reg | ✅ | M6.139 | |
| `CLR (An)+ / (xxx)` | .L / .W | mem | ⚡ / 🐢 | M6.130 / M6.161 | `(An)+` form covered; M6.161 swaps slow path to `m68k_jit_clr_w_anpi_mmio` |
| `TST Dn` | .B / .W / .L | reg | ✅ | M6.101 / M6.138 | M6.95 / M6.325 fusion with Bcc.S |
| `TST (An)` | .B | mem | 🐢 | — | **M6.148 attempt reverted** (bridge trajectory shift) |
| `TST (xxx).W` | .B / .W / .L | abs | 🪝 | M6.131 | helper narrowed mask |

## Bit operations (BTST / BCHG / BCLR / BSET)

68000 splits these by source (#imm static vs Dm dynamic) and by destination (Dn / EA).

| Form | Style | Milestone | Notes |
|------|:-----:|----------:|-------|
| `BTST #imm,Dn` | ✅ | M6.154 | sibling family with BCHG/BCLR/BSET |
| `BTST Dm,Dn` | ✅ | M5.2 | |
| `BTST #imm,(d16,An)` | ⚡ | M6.31 | `m68k_jit_btst_b_mmio` |
| `BTST #imm,(xxx).W` | ⚡ | M6.113 | |
| `BTST Dm,(EA)` | 🐢 | — | dynamic-Dm to memory |
| `BCHG / BCLR / BSET #imm,Dn` | ✅ | M6.154 | bench/boot saw 390 fires/100M on 0x08C0 (BSET #imm,D0) |
| `BSET / BCLR / BCHG Dm,Dn` | 🐢 | — | dynamic-Dm form |
| `BSET / BCLR / BCHG #imm,(xxx).W` | ⚡ | M6.113 | static-imm to abs.W |
| `BSET / BCLR / BCHG #imm,(EA)` | 🐢 | — | other memory modes |

## Shift / rotate

The whole family follows the pattern `0xExxx`; size is bits 7-6, direction
is bit 8, the operator is bits 4-3 (00 = AS, 01 = LS, 10 = ROX, 11 = RO),
and bit 5 selects immediate vs Dm-source count.

| Mnemonic | Direction | Sizes | Style | Milestone | Notes |
|----------|:---------:|:-----:|:-----:|----------:|-------|
| `ASR #imm,Dn` | right | .B / .W / .L | ✅ | M6.97-99 | |
| `ASL #imm,Dn` | left | .B / .W | ✅ | M6.151 | V flag via top-bits XOR self-test |
| `ASL.L #imm,Dn` | left | .L | ✅ | M6.155 | sibling of M6.151; in-place 32-bit |
| `LSR #imm,Dn` | right | .B / .W / .L | ✅ | M6.97-99 | |
| `LSL #imm,Dn` | left | .B / .W / .L | ✅ | M6.146 | |
| `ROR #imm,Dn` | right | .B / .W / .L | ✅ | M6.150 | |
| `ROL #imm,Dn` | left | .B / .W / .L | ✅ | M6.150 | |
| `ROXR #imm,Dn` | right | .B / .W / .L | ✅ | M6.143 | X-rotating, per-iter X update |
| `ROXL #imm,Dn` | left | .B / .W / .L | ✅ | M6.149 | sibling of M6.143 |
| **Dm-source count** (all 8 above) | — | — | 🐢 | — | bench-hot 0xEAA8 LSR.L D5,D0 (190 fires) — variable-count is harder |
| **Memory destination (EA)** | — | — | 🐢 | — | `ROR (xxx)` etc. rarely fire on Mac |

## Compare

| Mnemonic | Sizes | Mode | Style | Milestone | Notes |
|----------|:-----:|------|:-----:|----------:|-------|
| `CMP Dm,Dn` | .B / .W / .L | reg-reg | ✅ | M5.4 / M6.135 | |
| `CMP (An),Dn` | .B / .W / .L | mem→reg | ⚡ | M6.129 | bench inner-loop |
| `CMP (d16,An),Dn` | .W | (d16,An)→Dn | ⚡ | M6.87 / M6.129 | decode pitfall doc'd |
| `CMP (An)+,Dn` | — | postinc | 🐢 | — | bench-hot but bridge-traj-risky |
| `CMPA.W (d16,An),An` | .W | mem→An | ⚡ | M6.129 | |
| `CMPA.L Dm,An` | .L | reg-An | ✅ | M5.4 | |
| `CMPI.W #imm,(d16,An)` | .W | imm vs mem | ⚡ | M6.129 | |
| `CMPI #imm,Dn` | .B / .W / .L | imm vs reg | ✅ | M5.2 | |
| `CMPM (An)+,(Am)+` | — | — | 🐢 | — | |

## Control flow

| Mnemonic | Variant | Style | Milestone | Notes |
|----------|---------|:-----:|----------:|-------|
| `BRA.S disp8` | short | ✅ | M5.4 | |
| `BRA.W disp16` | word | ✅ | M6.106 / M6.140 | |
| `Bcc.S disp8` (all 14 cc) | short | ✅ | M5.4 / M5.9 | |
| `Bcc.S` fused with prior TST/MOVE | branchless tail | 🔗 | M6.95 / M6.147 | `emit_bcc_branchless_tail` |
| `Bcc.W disp16` | word | ✅ | M6.106 / M6.140 | disp16 off-by-2 caught by tests |
| `BSR.S disp8` | short | ✅ | M5.4 | M6.132: fast helper for SP→MMIO |
| `BSR.W disp16` | word | ✅ | M6.105 | M6.132 fast helper |
| `JMP (An)` | reg-indirect | ✅ | M6.128 | block terminator |
| `JMP (d16,An)` | mem | ✅ | M6.140 | |
| `JMP (xxx).L` | abs | ✅ | M6.140 | M6.140 cross-block successor |
| `JMP (d16,PC)` | PC-rel | ✅ | M6.140 | |
| `JSR (An)` | reg-indirect | ✅ | M6.128 | boot 0x4E90 |
| `JSR (d16,An)` | | ✅ | M6.116 | |
| `JSR other modes` | | 🐢 | — | |
| `RTS` | | ✅ | M5.4 / M6.132 | fast helper for SP→MMIO |
| `RTR` | | 🐢 | — | rare, CCR-restoring return |
| `RTE` | | 🐢 | — | privileged exception return |
| `DBcc Dn,disp16` | all 16 cc | ✅ | M5.4 | `emit_bcc_branchless_tail` w/ count-decrement |
| `Scc Dn` | all 16 cc | ✅ | M6.141 | sets all-1s / all-0s in Dn[7:0] |

## System / exception

| Mnemonic | Style | Milestone | Notes |
|----------|:-----:|----------:|-------|
| `NOP` | ✅ | early | |
| `RESET` | 🐢 | — | rare; m68k_step halts cleanly |
| `STOP` | 🐢 | — | rare; m68k_step handles halt |
| `MOVE #imm,SR` | ⚡ | M6.117 | imm.S=1 fast path + slow bridge |
| `MOVE SR,Dn / MOVE to/from CCR` | 🐢 | — | |
| `TRAP #vector` | 🐢 | — | exception-fast-helper pattern fits |
| `CHK / TRAPV` | 🐢 | — | |
| `Line-A (0xAxxx)` | 🐢 | — | Toolbox traps — go through m68k_step |
| `Line-F (0xFxxx)` | ⚡ | M6.137 | `m68k_jit_fline_trap` (exception fast helper) |
| `Illegal` | 🐢 | — | |
| `Privilege violation` | 🐢 | — | |
| `Bus / address error` | 🐢 | — | |
| `Interrupt acknowledge` | 🐢 | — | |

## Cross-cutting compile-time analysis

These don't emit per-op runtime code but shape the JIT's behaviour:

| Feature | Style | Milestone | Notes |
|---------|:-----:|----------:|-------|
| Register cache (4 slots a4-a7) | 🧠 | M6.13 | picks top-4 most-used D/A regs per block; ≥99% hit rate steady-state |
| Cross-block cache survival | 🧠 | M6.62 | predecessor's cached regs survive into chained successor |
| Native chain JX (ESP32) | 🧠 | M6.54 | unmeasurable on host |
| Static-successor prefetch | 🧠 | M6.71 | prefetches BRA/Bcc/JMP statics |
| Chain successor prefetch | 🧠 | M6.72 | walks deeper chain |
| Lazy CCs | 🧠 | M6.114 | `classify_op` SET-only vs SET+CONS |
| `g_helper_touched_mask` narrowing | 🧠 | M6.123 | skips reload of slots a helper doesn't touch |
| Flags-dead detection | 🧠 | M6.88 | lean MOVE variant when next op is a setter |
| Cross-block JMP successor analysis | 🧠 | M6.140 | static `JMP (xxx).L` becomes a chain |
| MOVEA+ADDA fusion | 🧠 | M6.85 | absorbs cheap MOVEA into trailing ADDA |
| LEA(d8,An,Xn)+ADDA fusion | 🧠 | M6.85b | sibling fusion path |

---

## Remaining opportunities (informed shopping list)

Sorted by likely win, accounting for trajectory-safety
(see `memory/bridge-only-arms-trajectory-shift.md`):

### Pure register-op extensions (safe — extend an existing sibling)

| Candidate | Boot 100M fires | Bench fires | Why |
|-----------|----------------:|-------------:|-----|
| ~~`BSET / BCLR / BCHG #imm,Dn`~~ | ~~390~~ | — | ✅ M6.154 (-434 boot 100M helpers) |
| ~~`ASL.L #imm,Dn`~~ | ~~120~~ | — | ✅ M6.155 (-124 boot 100M helpers) |
| ~~`NOT Dn`~~ | — | ~~36~~ | ✅ M6.156 (-66 bench helpers) |
| ~~`SUBA.L Dn/Am,An`~~ | — | — | ✅ M6.157 (-11 bench, -6 boot helpers) |

**The pure-register-op shopping list is empty.** Remaining opportunities
are in the harder categories: dynamic-Dm shift count (`0xEAA8 LSR.L
D5,D0` 190 bench fires), multi-step fusion, and the structural items
(per-helper CCR mask). See the "Structural items" subsection.

### Compile-time fusion (safe — emits no new runtime code)

| Candidate | Notes |
|-----------|-------|
| `CMPI #imm,Dn + Bcc.S` | Sibling of M6.95 TST.L+Bcc fusion |
| `EXT.L + Bcc.S` | Sets N from bit 31 — same fusion shape |

### Slow-path bridge conversion (safe — existing arm, swap helper)

| Candidate | Notes |
|-----------|-------|
| `MOVE.W (An)+,Dn` MMIO | Existing arm has m68k_step bridge; converting to fast helper is M6.144-shape |
| `CLR.L (An)+` MMIO | M6.130 has the fast path; bridge can be tightened |

### Risky (bridge-structure) — not recommended

| Candidate | Why it's risky |
|-----------|----------------|
| `OR.W Dn,(An)` | M6.145 attempt regressed boot 100M +2.2% (strictly-absent pattern still shifted trajectory) |
| `TST.B (An)` | M6.148 attempt regressed boot 100M +32% (new arm with bridge structure) |
| Any **brand-new arm with bounds-check + slow-path bridge** | See `memory/bridge-only-arms-trajectory-shift.md` |

### Structural items (the big ones)

| Item | State | Expected win |
|------|-------|--------------|
| **Per-helper CCR-read/write mask** | Item 2 in roadmap — untouched | Skip `emit_sr_flush` before helpers that don't read SR; skip `emit_sr_reload` after helpers that don't write CCR. Estimated 2-5 LX7 per affected helper bridge. |
| **Trampoline-preserves-a4..a7 around fast helpers** | Item 1 sub-item | Skip cache flush/reload of slots a helper doesn't touch. Per-bridge: 2 × |touched_slots| LX7 |
| **Native chain on ESP32** | Item 3 — infrastructure ready | ~5-15 LX7 per chained block on real hardware; unmeasurable on host |
| **M6.66 root-cause fix** | open | VIA tick granularity gap between JIT and interp causes boot 100M chaotic-trajectory swings; fixing it would unblock many trajectory-locked arms |

---

## How this file gets updated

When a new inline arm or fast helper lands, add a row to the appropriate
table and update the "Coverage at a glance" counts. Each new milestone
should also pick up a STATUS.md log entry, but this file is the
quick-reference index — keep it dense and current.

If a row's Style changes (e.g. m68k_step → ⚡ inline+helper), update the
"Remaining opportunities" lists too — they're the project's shopping list,
not just a static report.

---

## M6.169+ delta (summary)

Twenty-five thinkc8 inline arms (M6.169-M6.193) plus seven boot-tuning
arms (M6.196, M6.198-M6.200, M6.202, M6.204) and tooling (M6.197, M6.201,
M6.203, M6.205) landed since the M6.161 bulk-table state. Rather than
re-flow every row, this section catalogs which opcode rows above are
now updated.

### Thinkc8 series (M6.169-M6.193)

Inline arms targeting thinkc8-folder-open bench helpers. All
trajectory-safe per [[bridge-only-arms-trajectory-shift]] absence-from-
boot-100M predicate.

| MS | Opcode shape | Replaces "🐢" row |
|----|--------------|-------------------|
| M6.169 | `4A28` TST.B (d16,An) | (new ⚡) |
| M6.170 | `B1ED` CMPA.L (d16,An),An | (new ✅) |
| M6.171 | `302D 322D` MOVE.W (d16,An),Dn | extends MOVE.W (d16,An) ✅ |
| M6.172 | `0C50` CMPI.W #imm,(An) | extends CMPI.W ⚡ |
| M6.173 | `4E58` UNLK An | `UNLK` row now ✅ |
| M6.174 | `4E50` LINK An,#d16 | `LINK` row now ✅ |
| M6.175 | `2178` MOVE.L (xxx).W,(d16,An) | (new ⚡) |
| M6.176 | `200D` MOVE.L Am,Dn | (new ✅) |
| M6.177 | `4A6D` TST.W (d16,An) | (new ⚡) |
| M6.178 | `B0AD` CMP.L (d16,An),Dn | (new ✅) |
| M6.179 | `4A78` TST.W (xxx).W | (new ✅) |
| M6.180 | `B0B8` CMP.L (xxx).W,Dn | (new ✅) |
| M6.181 | `4250` CLR.W (An) | extends CLR.W mem ⚡ |
| M6.182 | `42A8` CLR.L (d16,An) | (new ⚡) |
| M6.183 | `C078` AND.W (xxx).W,Dn | (new ✅) |
| M6.184 | `486E` PEA (d16,An) | `PEA` row partly ⚡ now |
| M6.185 | `2F38` MOVE.L (xxx).W,-(An) | (new ⚡) |
| M6.186 | `2472` MOVEA.L (d8,An,Xn),Am | first (d8,An,Xn) ⚡ |
| M6.187 | `0C70` CMPI.W #imm,(d8,An,Xn) | second (d8,An,Xn) ⚡ |
| M6.188 | `1141` MOVE.B Dn,(d16,An) | extends MOVE.B Dn,mem ⚡ |
| M6.189 | `1178` MOVE.B (xxx).W,(d16,An) | (new ⚡) |
| M6.190 | `A0xx` line-A trap | `TRAP` family — now ⚡ (m68k_jit_aline_trap) |
| M6.191 | `007C` ORI.W #imm,SR (S=0 only) | `ORI to SR` row partly ✅ |
| M6.192 | `40E7` MOVE SR,-(An) | (new ⚡) |
| M6.193 | `46DF` MOVE (An)+,SR | (new ⚡ m68k_jit_move_anpi_to_sr) |

Effect: thinkc8 helpers dropped 2 093 K → 0 (−100%); other benches
unchanged.

### Boot tuning series (M6.196-M6.204)

| MS | Opcode shape | Impact |
|----|--------------|--------|
| M6.196 | CMP.L (d16,An),Dn + Bcc.S/W fusion | thinkc8 −149K xt |
| M6.198 | RTE inline (m68k_jit_rte fast helper) | boot 100M real_helpers −142 |
| M6.199 | ADDQ.L #imm,(xxx).W with skip_flags | boot 100M helpers −320 |
| M6.200 | MOVE.B (xxx).L,Dn | boot 100M real_helpers −196 |
| M6.202 | BTST/BCHG/BCLR/BSET Dn,(An) variable-shift mask | boot 100M helpers −893 |
| M6.204 | M6.202 MMIO slow-path → m68k_jit_bitop_dn_an_mmio | boot 100M real_helpers −831 |

### Tooling (M6.197 / M6.201 / M6.203 / M6.205)

| MS | What |
|----|------|
| M6.197 | Add boot-cycle100m.snap (5th bench target) |
| M6.201 | Extend boot-snap diff_jit_trace window 11K → 100K cycles |
| M6.203 | Add boot-cycle30m.snap (6th bench target) |
| M6.205 | Widen helper-histo dump 20 → 40 entries |

### Current saturation state (post-M6.205)

See `memory/host-perf-saturation.md` for the saturation analysis. The
host-measurable frontier is genuinely saturated; further wins require
deeper architectural moves (variable-count shifts in the encoder,
native ESP32 chain measurement, or M6.66 root-cause fix).
