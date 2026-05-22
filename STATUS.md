# Status

## Summary

Milestones M1–M4 are complete: a working 68000 interpreter, a working
68000→Xtensa JIT, and the full emulator running with the CPU executing
as native Xtensa code under `qemu-system-xtensa`.

Beyond the original scope, the emulator **boots a real Macintosh Plus
ROM with System Software 6.0.8 all the way to the Finder desktop** —
under both the interpreter and the JIT. See the M6 notes below.

```
$ ctest --test-dir build
    interp ............ Passed
    encoder ........... Passed
    jit_differential .. Passed          3/3

$ ./scripts/run_qemu_s3.sh
    [BENCH] mode=interp cycles=8738 mhz=3.425 pc=0x00019E exit=0
    [BENCH] mode=jit    cycles=8738 mhz=1.668 pc=0x00019E exit=0
                        blocks=14/262 inline_ops=29 helper_ops=25 chain=245/17
    RESULT: PASS
```

`mode=interp` and `mode=jit` reach byte-identical state (registers,
flags, cycle count, framebuffer checksum) — the JIT is verified correct
against the interpreter oracle, on the host and on the emulated LX7.

## What is done

### M1 — 68000 interpreter (`core/m68k_interp.c`)
Covers the bulk of the user/supervisor integer ISA: all 12 EA modes,
MOVE/MOVEA/MOVEQ, the immediate and register ALU groups, ADD/SUB/AND/OR/
EOR/CMP in every form, ADDA/SUBA/CMPA, ADDQ/SUBQ, the shift/rotate
family, Bcc/BRA/BSR/DBcc/Scc, JMP/JSR/RTS/RTR/RTE, BTST/BCHG/BCLR/BSET,
MULU/MULS/DIVU/DIVS, MOVEM, LEA/PEA, EXT/SWAP/LINK/UNLK, NEG/NEGX/NOT/
CLR/TST/TAS, TRAP, exceptions and autovector interrupts.

### M2 — Mac memory & peripherals (`core/mac_mem.c`)
24-bit memory map, ROM boot overlay, a reduced 6522 VIA (T1 timer +
~60 Hz vertical-blank interrupt to IPL 1), a 1bpp framebuffer, and the
harness debug serial/exit ports.

### M3 — basic JIT (`jit/`)
Block discovery, native Xtensa emission for the inline fast set with
full CCR computation, `CALLX0`-to-interpreter fallback for the rest, a
hash-table block cache and a single-slot next-block predictor. The
inline set: NOP, MOVEQ, MOVE.L #imm/Dm/→Dn, MOVEA.L #imm, ADD.L,
ADDQ.L/SUBQ.L, ADDQ/SUBQ→An, CMP.L. On the demo, 29 of 54 distinct
op-emissions are native; the rest fall back.

### M4 — JIT on ESP32-S3 under QEMU (`port/esp32s3/`)
Executable IRAM code-cache arena (`MALLOC_CAP_EXEC`), the hand-written
CALL0⇄windowed trampoline, and native block execution on the emulated
LX7. The firmware runs the demo under both engines and prints `PASS`.

## Bugs found & fixed during bring-up

- **Decoder/JIT length disagreement.** `m68k_decode_at` mis-sized
  case-0x4 instructions with no effective-address field (NOP, SWAP,
  EXT, …) — it read their register bits as an EA mode and added phantom
  extension words. The JIT discovered the wrong opcode sequence while
  the inline ops advanced the real PC correctly, desyncing the two.
  Fixed by giving the no-EA forms explicit lengths.
- **JIT vs interpreter halt timing.** The interpreter's run loop stops
  the instant `cpu->halted` is set; a JIT block may contain later
  instructions. `m68k_step` now early-returns when halted, so both
  engines end in the same state.

## Known limitations / future work (M5–M6)

- **The JIT does not yet beat the interpreter on tight loops.** A basic
  block JIT pays a full dispatch round-trip (windowed→CALL0 entry,
  prologue, epilogue, return) *per loop iteration*, plus a `CALLX0` for
  any non-inlined op such as the loop's terminating branch. On the
  demo's two short hot loops that overhead exceeds the interpreter's
  switch-table dispatch. This is the expected behaviour of the M3/M4
  milestone — `gbjit-xtensa`'s own benchmarks show the same crossover.
  The fix is **M5**: native block chaining (a block's tail jumps
  straight to its successor) and **back-edge internalisation** (keep a
  loop inside a single block with an internal Xtensa branch), then a
  wider inline opcode set and inline memory fast-paths.
- **Inline opcode coverage is partial.** Branches, memory MOVEs, LEA,
  the bit ops and the shift family still fall back to the interpreter.
- **Real Macintosh ROM — boots to the Finder.** With a Macintosh Plus
  v3 ROM (128 KB, checksum 4D1F8172) and a System 6.0.8 800 KB floppy:

  ```
  ./build/mac68k_host --jit --rom --disk roms/disks/ssw608_d1.img \
      --screenshot boot.bmp "roms/...MacPlus v3.ROM"
  ```

  The ROM runs its full power-on self test (RAM sizing, ROM checksum,
  the VBL-driven `Ticks` sync), clears the boot overlay, dispatches the
  Macintosh Toolbox A-line traps, initialises QuickDraw, reads the boot
  blocks off the floppy, loads the System and Finder, and reaches the
  **Finder desktop** — menu bar, the mounted "System Tools" disk icon,
  the trash, the cursor. The JIT boots it identically to the
  interpreter.

  What this took:
  - **Mac Plus peripherals** modelled from the hardware up — the 6522
    VIA (timers, ~60 Hz VBL interrupt, ROM overlay on PA4, video page),
    the Apple RTC, reduced SCC / NCR 5380 SCSI (`core/mac_mem.c`).
  - **Two real CPU bugs** the ROM exposed: line-A traps were routed to
    the illegal vector instead of vector 10 (the whole Toolbox is line-A
    traps); and MOVEP was unimplemented and mis-decoded as a bit op,
    desyncing the instruction stream.
  - **The `.Sony` floppy driver**, ported from mini vMac
    (`third_party/minivmac`, a git submodule): rather than emulate the
    IWM/GCR at the bit level, the ROM's disk driver is replaced with
    mini vMac's 68k stub that traps into the emulator, which serves
    whole logical sectors from the disk image (`core/sony.c`).
  - **JIT self-modifying-code invalidation** (`jit/dispatcher.c`): the
    OS loads code segments into RAM and reuses that RAM, so the JIT now
    tracks which 256-byte pages hold compiled code and drops the
    affected blocks when the guest writes them. Without this the JIT
    ran stale blocks and the OS bombed with "unimplemented trap".

  The earlier IWM-level GCR path (`core/mac_iwm.c`) is superseded by the
  `.Sony` driver patch and left in place only as the RTC host / IWM
  register stub.
- **Peripherals are reduced.** Only the VIA timer + VBL interrupt are
  modelled; IWM (disk), SCC (serial), ADB/keyboard and sound are not.
- **Cycle counts are approximate** — good enough to pace the ~60 Hz VBL,
  not instruction-cycle-accurate.
