# mac68k-jit-xtensa

A Motorola 68000 Macintosh emulator whose CPU core is **JIT-compiled to
Xtensa LX7 machine code** — targeting the ESP32-S3 microcontroller.

68000 instructions are translated at runtime into native Xtensa code that
runs directly on the LX7 core; the generated blocks live in an executable
IRAM code cache and are entered through a CALL0 ⇄ windowed-ABI bridge. This
mirrors the design of the sister project [`gbjit-xtensa`](../gbjit-xtensa)
(a Game Boy JIT), reusing its Xtensa encoder, code cache and host Xtensa
simulator.

```
mac68k: boot — mac68k-jit-xtensa (68000 Macintosh CPU on Xtensa LX7)
RESULT: PASS
[BENCH] mode=interp cycles=8738 mhz=3.425 pc=0x00019E exit=0 fbsum=0xD368A000
RESULT: PASS
[BENCH] mode=jit    cycles=8738 mhz=1.668 pc=0x00019E exit=0 fbsum=0xD368A000
        blocks=14/262 inline_ops=29 helper_ops=25 chain=245/17
RESULT: PASS
```

The CPU above is running under **`qemu-system-xtensa`** emulating a real
ESP32-S3 — the 68000 guest program is translated to Xtensa and executed
natively by the emulated LX7.

## What works

- **Reference 68000 interpreter** (`core/m68k_interp.c`) — covers the bulk
  of the user/supervisor integer ISA: every effective-addressing mode,
  the MOVE family, the immediate and register ALU groups, ADD/SUB/AND/OR/
  EOR/CMP in all forms, ADDQ/SUBQ, the shift/rotate family, Bcc/BRA/BSR/
  DBcc/Scc, JMP/JSR/RTS/RTR/RTE, the bit ops, MUL/DIV, MOVEM, LEA/PEA,
  EXT/SWAP/LINK/UNLK, NEG/NEGX/NOT/CLR/TST/TAS, TRAP and autovector
  interrupts. It is the correctness oracle.
- **Basic JIT** (`jit/codegen.c`, `jit/dispatcher.c`) — discovers basic
  blocks, emits native Xtensa for a curated fast set of opcodes
  (MOVE/MOVEQ/MOVEA, ADD/ADDQ/SUBQ, CMP, NOP) with full CCR computation,
  and falls back to a `CALLX0` into the interpreter for everything else.
  A single-slot next-block predictor chains hot blocks.
- **Mac Plus peripheral model** (`core/mac_mem.c`, `core/mac_iwm.c`) —
  the Macintosh memory map with the boot ROM overlay, the 6522 VIA
  (timers, ~60 Hz vertical-blank interrupt, video-page select), the
  Apple RTC, the IWM floppy controller with Apple 3.5" GCR synthesis,
  and reduced SCC / NCR 5380 SCSI. mini vMac is the reference.
- **Boots a real Macintosh Plus ROM with System 6.0.8 to the Finder
  desktop** — under both the interpreter and the JIT. Full power-on
  self test, Toolbox trap dispatch, QuickDraw, an 800 KB floppy boot
  via mini vMac's `.Sony` driver patch (`third_party/minivmac`
  submodule), and JIT self-modifying-code invalidation for the OS's
  RAM-resident code. See [STATUS.md](STATUS.md).
- **Two execution paths from one codebase** — the host build runs JIT
  output through an in-tree Xtensa simulator (`jit/xtensa_sim.c`); the
  ESP32-S3 build runs it natively. Both are checked by the test suite.

A built-in demo ROM (`core/demo_rom.c`, assembled in C — there is no host
68k toolchain) stands in for a copyrighted Macintosh boot ROM. It sums
1..100, exercises the shift/logic ops, calls a subroutine, fills the
framebuffer and prints a result line.

See [STATUS.md](STATUS.md) for the milestone breakdown and the known
limitations, and [PLAN.md](PLAN.md) for the design.

## Quick start — host

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure        # 3/3

./build/mac68k_host --interp        # run the demo under the interpreter
./build/mac68k_host --jit           # run it through the JIT (host Xtensa sim)
./build/mac68k_host --jit rom.bin   # run an arbitrary raw 68k image at 0x0

# Boot a real Macintosh Plus ROM (mapped at 0x400000, overlaid for boot),
# optionally with an 800K floppy, and snapshot the screen:
./build/mac68k_host --rom --disk floppy.img --screenshot screen.bmp \
                    --max-cycles 700000000 MacPlus.ROM
```

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
tests/          ctest cases
scripts/        QEMU runner + IDF environment helper
third_party/    minivmac submodule (source of the .Sony driver patch)
```

## Tests

```
$ ctest --test-dir build
    interp           — interpreter runs a hand-built snippet + the demo
    encoder          — Xtensa encoder output round-trips through the sim
    jit_differential — JIT register/flag/cycle state matches the interpreter
```
