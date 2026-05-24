# mac68k-jit-xtensa

A Motorola 68000 Macintosh emulator whose CPU core is **JIT-compiled to
Xtensa LX7 machine code** — targeting the ESP32-S3 microcontroller.

68000 instructions are translated at runtime into native Xtensa code that
runs directly on the LX7 core; the generated blocks live in an executable
IRAM code cache and are entered through a CALL0 ⇄ windowed-ABI bridge. This
mirrors the design of the sister project [`gbjit-xtensa`](../gbjit-xtensa)
(a Game Boy JIT), reusing its Xtensa encoder, code cache and host Xtensa
simulator.

## Headline numbers (host, JIT vs interp)

| Workload | interp lx7/cyc | JIT lx7/cyc | speedup |
|---|---:|---:|---:|
| Speedometer (frozen snapshot) | 6.462 | **1.279** | **5.05 ×** |
| Mac Plus ROM boot (100 M cyc) | 5.895 | **1.735** | 3.40 × |

The bench exceeds the original "5 × interp" goal (M6.31, M6.51). The
boot number is path-dependent on VIA-tick timing — see `STATUS.md`.

On real ESP32-S3 hardware the additional native-chaining (M6.54) and
cross-block register-caching (M6.62) optimisations add an estimated
~12 % + ~3 % on boot — both untestable via the host Xtensa sim, which
runs one block per invocation.

## Quick start

The `scripts/` helpers wrap the most common entry points:

```sh
./scripts/build.sh                  # cmake + build, host Release
./scripts/test.sh                   # ctest — 4/4 incl. diff-jit lockstep
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
  basic blocks, emits native Xtensa for ~30 hot opcodes / patterns
  including MOVE, MOVEQ, MOVEA, ADD / ADDA / ADDQ, SUB / SUBA / SUBQ,
  CMP / CMPI / CMPM, JSR / JMP / RTS (with in-RAM fast paths),
  Bcc.S / DBcc, fused-CMP+Bcc, MOVE.L push / pop patterns, MOVEM
  predec / postinc, BTST / ORI MMIO fast helpers. Everything else
  falls back to a `CALLX0` into `m68k_step`. Full X N Z V C
  computation with lazy CC liveness.
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
                diff_jit_bench_lockstep)
scripts/        build / test / gui / boot / bench / diff helpers plus
                QEMU runner and IDF environment helper
third_party/    minivmac submodule (source of the .Sony driver patch)
```

## Tests

```
$ ctest --test-dir build
    interp                            — interpreter runs a hand-built snippet + the demo
    encoder                           — Xtensa encoder output round-trips through the sim
    jit_differential                  — JIT register/flag/cycle state matches the interp
    prefetch                          — m68k_block_static_successors unit tests (M6.71)
    diff_jit_bench_lockstep           — JIT/interp lockstep on speedo-bench.snap to
                                        cycle 11000 (M6.68/M6.69 SR-flush regression
                                        guard). Conditional on the snapshot's presence.
    diff_jit_bench_lockstep_prefetch  — same lockstep with --prefetch static (M6.71)
    diff_jit_bench_lockstep_prefetch_chain
                                      — same lockstep with --prefetch chain (M6.72)
```

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
