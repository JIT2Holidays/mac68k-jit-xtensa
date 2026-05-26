# mac68k-jit-xtensa

A Motorola 68000 Macintosh emulator whose CPU core is **JIT-compiled to
Xtensa LX7 machine code** — targeting the ESP32-S3 microcontroller.

68000 instructions are translated at runtime into native Xtensa code that
runs directly on the LX7 core; the generated blocks live in an executable
IRAM code cache and are entered through a CALL0 ⇄ windowed-ABI bridge. This
mirrors the design of the sister project [`gb-jit-xtensa`](https://github.com/JIT2Holidays/gb-jit-xtensa)
(a Game Boy JIT), reusing its Xtensa encoder, code cache and host Xtensa
simulator.

## Headline numbers (host, JIT vs interp)

State at **M6.243**. Eight bench targets exercise distinct opcode mixes
(see the full table at the top of `STATUS.md`):

| Workload | JIT lx7/cyc | real_helpers/100M |
|---|---:|---:|
| Speedometer 100 M (frozen snapshot) | **1.179** | 1 237 |
| boot-cycle30m (Toolbox init) | **1.334** | 8 |
| thinkc8-folder-open (Finder w/ THINK C 8 folder) | **1.389** | 0 |
| boot-cycle100m (mid INIT/extension load) | **1.634** | 1 422 |
| Mac Plus ROM boot 100 M (live) | **1.656** | 175 726 |
| boot-rom-init (ROM memory test) | **1.662** | 232 106 |
| boot-system-load (post-System-load) | **1.786** | **0** |
| thinkc-bullseye (THINK C 5.0 IDE, Bullseye demo) | **2.155** | 984 959 |

Interp baseline is ~6 lx7/cyc; the bench is **5.48 × interp**, well past
the original 5 × goal (M6.31, M6.51). Two snapshots (`thinkc8-folder-open`,
`boot-system-load`) reached **helpers = 0** — every opcode in those frozen
windows is inline. Boot numbers are path-dependent on VIA-tick timing —
see `STATUS.md` and `memory/m6.66-trajectory-traps.md`.

For a per-instruction view of what's inline vs what still falls to
`m68k_step`, see [`INSTRUCTIONS.md`](INSTRUCTIONS.md).

On real ESP32-S3 hardware the additional native-chaining (M6.54) and
cross-block register-caching (M6.62) optimisations add an estimated
~12 % + ~3 % on boot — both untestable via the host Xtensa sim, which
runs one block per invocation.

## Quick start

The `scripts/` helpers wrap the most common entry points:

```sh
./scripts/build.sh                  # cmake + build, host Release
./scripts/test.sh                   # ctest — 9/9 incl. diff-jit lockstep
./scripts/gui.sh                    # SDL GUI: Mac Plus ROM + System 6 floppy
./scripts/boot.sh jit               # headless boot, 100 M cycles, prints perf
./scripts/bench.sh jit              # Speedometer snapshot — JIT cost metric
./scripts/diff.sh                   # JIT vs interp lockstep (correctness)
```

Each script `--help`-style block sits in its first comment. Direct
`mac68k_host` invocation:

```sh
./build/mac68k_host --interp        # demo ROM under the interpreter
./build/mac68k_host --jit           # demo ROM through the JIT (host Xtensa sim)
./build/mac68k_host --jit rom.bin   # arbitrary raw 68k image at 0x0

# Boot a real Macintosh Plus ROM, an 800K floppy, a 1 GB Infinite HD as
# drive 2 (auto-inserted post-boot), with a tuned JIT arena + LRU evict
# and static-successor prefetch on:
./build/mac68k_host --jit --rom roms/MacPlus.ROM \
    --disk roms/disks/System6.dsk \
    --evict lru --arena-kb 256 \
    --prefetch static \
    --max-cycles 100000000
```

JIT-tuning flags:
* `--arena-kb N`            — JIT codecache arena size (default 1024).
* `--evict none|lru|fifo`   — eviction policy on arena fill (M6.63).
* `--prefetch none|static|chain` — speculative compile of statically-
  known block successors. `static` (M6.71) prefetches both branches
  of every Bcc, depth 1 — wastes ~80 % of prefetches. `chain` (M6.72)
  follows only unambiguous successors (BRA / BSR / JMP / fall-through)
  to depth `--prefetch-depth N` (default 2) — no wasted compiles,
  same first-order amortisation. Off by default.
* `--prefetch-depth N`       — CHAIN-mode follow depth (default 2).

## What works

- **Reference 68000 interpreter** (`core/m68k_interp.c`) — full user /
  supervisor integer ISA: every effective-addressing mode, the MOVE /
  ALU / shift / branch / bit families, MUL / DIV, MOVEM, LEA / PEA,
  EXT / SWAP / LINK / UNLK, NEG / NEGX / NOT / CLR / TST / TAS, TRAP,
  exceptions, autovector interrupts. It is the correctness oracle.
- **Inline-heavy JIT** (`jit/codegen.c`, `jit/dispatcher.c`) — discovers
  basic blocks and emits native Xtensa for **~150 opcode classes** across
  the MOVE / MOVEA / MOVEM / MOVEQ / LEA / PEA / LINK / UNLK, ADD / ADDA /
  ADDI / ADDQ / ADDX, SUB / SUBA / SUBI / SUBQ / SUBX, MUL, NEG / NEGX,
  EXT, CMP / CMPI / CMPM / CMPA, AND / OR / EOR / NOT / CLR / TST, full
  static-imm BTST / BCHG / BCLR / BSET to Dn / (An) / (d16,An) / (xxx).W
  plus dynamic-Dm bit ops to (An), ASR / ASL / LSR / LSL / ROR / ROL /
  ROXR / ROXL #imm shift family, ABCD / SBCD / NBCD BCD chain, Bcc /
  BRA / BSR / JMP / JSR / RTS / RTE / DBcc / Scc control family, line-A
  + line-F traps, MOVE-SR / MOVE-from-SR / ORI-to-SR system family.
  **About 30 custom `m68k_jit_*` fast helpers** cover bench/boot MMIO
  patterns (RTS / BSR / JSR / MOVEM (An)+ / MOVE-L address forms / MOVE-W
  address forms / MOVE-B address forms / MOVE-mem-to-mem .L+.W+.B /
  MOVEA-L+W / CMP.W (addr),Dn / CLR.W (An)+ / line-A + line-F traps / …)
  in place of `m68k_step`. Everything still uncovered (DIV, EXG, full
  Bxxx-Dm dynamic-source, full shift-by-Dm count, address-error trap,
  RTR …) falls back to a `CALLX0` into `m68k_step`. Full X / N / Z / V /
  C computation with lazy-CC liveness. See [`INSTRUCTIONS.md`](INSTRUCTIONS.md)
  for the per-instruction map.
- **Within-block + cross-block register caching** — hot D / A regs
  are cached in `a4..a7` for the lifetime of a block (M6.10), and
  the ESP32 chain epilogue can skip the next block's prologue
  entirely when its cache config matches the predecessor's (M6.62,
  99.7 % of boot's chain hits qualify).
- **Native block chaining (ESP32 only)** — block epilogue JX's
  directly to the predicted successor's entry, bypassing the
  dispatcher round-trip; `chain_budget` bounds the chain depth to
  keep VIA / IRQ latency under ~1 ms (M6.54). Host metric is
  unchanged (the simulator can't follow a JX out of the current
  block's code memory).
- **Mac Plus peripheral model** (`core/mac_mem.c`, `core/mac_iwm.c`)
  — the 24-bit memory map with ROM overlay, a 6522 VIA (T1 + ~60 Hz
  VBL), the Apple RTC, the IWM floppy controller with Apple 3.5″
  GCR synthesis, and reduced SCC / NCR 5380 SCSI. mini vMac is the
  reference.
- **Boots a real Macintosh Plus ROM with System 6.0.8 to the
  Finder desktop** — under both the interpreter and the JIT. Full
  power-on self test, Toolbox trap dispatch, QuickDraw, 800 KB
  floppy boot via mini vMac's `.Sony` driver patch
  (`third_party/minivmac` submodule), JIT self-modifying-code
  invalidation for the OS's RAM-resident code, ABCD / SBCD / NBCD
  for Control-Panel rendering.
- **Runtime-selectable code-cache eviction** — `--evict none|lru|fifo`
  with `--arena-kb N` lets the codecache run with bump (default),
  per-block LRU, or circular-FIFO eviction. M6.63 arena sweep shows
  bump suffers a 35 % cliff under 512 KB while LRU/FIFO hold flat;
  see `scripts/evict_sweep.png`.
- **Two execution paths from one codebase** — the host build runs
  JIT output through an in-tree Xtensa simulator (`jit/xtensa_sim.c`);
  the ESP32-S3 build runs it natively. Both are checked by `ctest`.

A built-in demo ROM (`core/demo_rom.c`, assembled in C — there is no host
68k toolchain) stands in for a copyrighted Macintosh boot ROM. It sums
1..100, exercises the shift / logic ops, calls a subroutine, fills the
framebuffer and prints a result line.

See `STATUS.md` for the per-milestone breakdown and known limitations,
and `PLAN.md` for the design.

## Quick start — ESP32-S3 under QEMU

Needs ESP-IDF v6.x with its bundled Xtensa toolchain and
`qemu-system-xtensa` (both ship with the IDF installer).

```sh
./scripts/run_qemu_s3.sh            # build + merge + qemu; exits 0 on PASS
```

`scripts/idf_env.sh` brings up the IDF environment (it works around a
Python-venv path mismatch in the stock `export.sh`).

## Layout

```
include/        shared types
core/           portable: 68000 interpreter, Mac memory/peripherals,
                the C-hosted 68k mini-assembler, the demo ROM
jit/            portable: Xtensa LX7 encoder, code cache, codegen,
                dispatcher, and (host only) an in-tree Xtensa simulator
port/host/      host CLI driver — mac68k_host
port/esp32s3/   ESP-IDF v6 project (component + app + trampolines)
gui/            SDL2 GUI front-end (mac_gui) — talks to host over a pipe
tests/          ctest cases (interp / encoder / jit_differential /
                prefetch / diff_jit lockstep — speedo bench × {plain,
                prefetch static, prefetch chain} + four boot-phase
                snapshots)
scripts/        build / test / gui / boot / bench / diff helpers plus
                QEMU runner and IDF environment helper
third_party/    minivmac submodule (source of the .Sony driver patch)
```

## Tests

```
$ ctest --test-dir build
    interp                                  — interpreter runs a hand-built snippet + the demo
    encoder                                 — Xtensa encoder output round-trips through the sim
    jit_differential                        — JIT register/flag/cycle state matches the interp
    prefetch                                — m68k_block_static_successors unit tests (M6.71)
    diff_jit_bench_lockstep                 — JIT/interp lockstep on speedo-bench.snap to
                                              cycle 11000 (M6.68/M6.69 SR-flush regression
                                              guard). Conditional on the snapshot's presence.
    diff_jit_bench_lockstep_prefetch        — same lockstep with --prefetch static (M6.71)
    diff_jit_bench_lockstep_prefetch_chain  — same lockstep with --prefetch chain (M6.72)
    diff_jit_boot_rom_init_lockstep         — lockstep on boot-rom-init.snap (M6.153)
    diff_jit_boot_system_load_lockstep      — lockstep on boot-system-load.snap (M6.153)
    diff_jit_boot_cycle100m_lockstep        — lockstep on boot-cycle100m.snap (M6.197)
    diff_jit_boot_cycle30m_lockstep         — lockstep on boot-cycle30m.snap (M6.203)
```

The `*.snap` lockstep tests are conditional on the snapshot files
existing in `roms/disks/`. The snapshots are gitignored (copyrighted
ROM bytes); regenerate them with `scripts/snap-extra-bench.sh` (for the
boot-phase snaps) — the Speedometer / thinkc8 / thinkc-bullseye snaps
are captured under the GUI via `MAC68K_SNAP`.

## Workflow notes

- **Triple-differential is SOP.** After any JIT-affecting change, run
  `./scripts/diff.sh` (or `--diff-jit-trace` directly) on a real
  workload. ctest's curated snippets miss bug classes — M6.68's
  SR-flush latent bug was present from M6.61 and only surfaced via
  `--diff-jit` on the Speedometer snapshot.
- **Auto-push at checkpoints.** Commits accumulate locally;
  `git push origin main` runs at milestone boundaries.
- **The boot host-perf metric is path-dependent** — VIA-tick
  granularity differs between engines, and Speedometer-style code
  paths can branch on VIA timer reads. The bench number (frozen
  Speedometer snapshot) is the cleaner signal.
