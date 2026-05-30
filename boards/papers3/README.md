# M5Stack PaperS3 board support

Headless bring-up of the mac68k-jit-xtensa emulator on the **M5Stack
PaperS3** (ESP32-S3, 16 MB flash, 8 MB octal PSRAM). This stage runs the
**CPU core (Xtensa JIT)** and **hard-disk simulation off the SD card**.
The e-ink display is intentionally *not* driven yet — the framebuffer
lives in guest RAM, so a panel driver added later only has to scan it out.

## What it does

1. Mounts the microSD card (FAT32) over SPI.
2. Loads `MacPlus.ROM` from the card into RAM and patches the `.Sony`
   driver (so disk I/O is served as logical sectors).
3. Attaches the two disk images as `.Sony` drives backed by **on-demand
   SD reads** — the images are never held in RAM, so a 10 MB System
   volume and a 1 GB Infinite HD work fine on 8 MB of PSRAM.
4. Resets the 68000 and runs it under the JIT, hot-inserting drive 2
   once the desktop is up.

## SD card layout (FAT32)

```
/MacPlus.ROM
/disks/System6.0.5.dsk     -> drive 1 (boot volume)
/disks/InfiniteHD6.dsk     -> drive 2 (inserted ~13 s after boot)
```

Copy these from the repo's `roms/` directory onto the card.

## Build & flash

```sh
source scripts/idf_env.sh          # ESP-IDF v6.0.1
cd boards/papers3
idf.py set-target esp32s3          # once
idf.py build
idf.py -p /dev/ttyACM0 flash monitor
```

Console output (UART0) shows the heartbeat: cycle count, guest PC and the
achieved emulated-MHz every 3 s, plus SD/disk mount logs.

## microSD pin map

The confirmed PaperS3 microSD (SPI) wiring is in `main/board_papers3.h`
(CS=47, SCK=39, MOSI=38, MISO=40). Override at build time if needed:

```sh
idf.py build -DBOARD_SD_CLK=39 -DBOARD_SD_MOSI=38 -DBOARD_SD_MISO=40 -DBOARD_SD_CS=47
```

## Validating the JIT (qemu, no hardware needed)

The Xtensa JIT path is exercised by a built-in self-test that runs the
demo under both the interpreter and the JIT and checks they agree:

```sh
bash boards/papers3/scripts_qemu_selftest.sh        # builds + runs under qemu-system-xtensa
# => "SELFTEST: PASS"
```

It builds with `-DBOARD_QEMU_SELFTEST=1` and a PSRAM-off overlay
(`sdkconfig.qemu`), since qemu's esp32s3 machine has neither octal PSRAM
nor an SD card. This is how the JIT was validated on real LX7 semantics
(qemu) — the host test suite only runs the JIT through the in-tree
software simulator, which cannot catch Xtensa-encoding/ABI bugs.

## Tunables

Compile-time defines (`-D…` or edit `app_main.c` / `board_papers3.h`):

| Define | Default | Meaning |
|--------|---------|---------|
| `BOARD_USE_JIT` | `1` | `1` = Xtensa JIT, `0` = reference interpreter (slower, always-correct fallback for first silicon bring-up) |
| `BOARD_RAM_MB` | `4` | Guest RAM (Mac Plus max 4 MB; allocated from PSRAM) |
| `BOARD_JIT_ARENA_KB` | `96` | IRAM JIT code-cache size. ~192 KB is the IRAM-EXEC ceiling; above that the alloc fails. Measured to NOT affect boot throughput (the boot working set far exceeds any feasible IRAM size). |
| `BOARD_JIT_EVICT` | `1` (LRU) | Code-cache eviction: 0=bump, 1=LRU, 2=FIFO, 3=random |
| `BOARD_JIT_HOT_THRESHOLD` | `8` | Hotspot gate: interpret a block until its entry pc is seen this many times, then JIT it. 0 = compile on first sight. Keeps the OS's run-once boot code interpreted (~1.75 MHz) instead of compile-thrashing it (~0.4 MHz). |
| `BOARD_HD_INSERT_CYCLE` | `100M` | Cycles after reset to insert drive 2 |

## Files

```
boards/papers3/
  CMakeLists.txt            top-level IDF project
  sdkconfig.defaults        PSRAM, 240 MHz, FAT-LFN, watchdogs off, memprot off
  partitions.csv            4 MB factory app (16 MB flash)
  main/
    app_main.c              SD mount -> ROM load -> attach disks -> run loop
    sd_disk.{c,h}           FAT32 mount + SD-streaming sony_disk_backend
    board_papers3.h         SD pin map + blob paths
  components/mac68k/
    CMakeLists.txt          full Mac core + JIT (references repo core/ & jit/)
    mac68k_target.c         IRAM arena + CALL0/windowed entry bridge
    jit_trampolines.S       m68k_step CALL0<->windowed shim
```

## Changes to shared code

Bringing the board up — making the Xtensa JIT correct on silicon, then
fast enough to boot — needed these additive changes outside `boards/papers3`.
The host build and all four host tests still pass (verified by `ctest` + a
host ROM-boot); the correctness-critical JIT fixes are `#if defined(ESP_PLATFORM)`,
and the shared ones (IRAM scratch, precise SMC, hotspot gate) preserve host
behavior (the gate defaults off; the others are behavior-neutral).

**Disk:**

1. **`core/sony.{c,h}` — streaming disk backend (`sony_attach_backend`).**
   A new optional sector read/write callback path. Drives attached the
   legacy way (`sony_insert_disk*`) keep a NULL backend and behave exactly
   as before.

**Xtensa JIT (the `#if defined(ESP_PLATFORM)` path had never been built or
run before — the host suite only exercises the in-tree software sim, which
shares the emitter's assumptions and so can't catch encoding/ABI bugs):**

2. **`jit/dispatcher.c` — `enter_block` signature.** The M6.82 chain-entry
   refactor added a `start_off` parameter to the host `enter_block` + the
   shared run loop but never updated the ESP variant, so the target hadn't
   compiled since. Added the parameter.

3. **`jit/dispatcher.c` — dispatcher always enters at the full prologue on
   ESP.** Skip-prologue entries (`body_addr`/`chain_entry_addr`) assume the
   guest-register cache is live in a4–a7; the host sim reloads it in C, but
   `m68k_enter_block` does a cold CALL0 and reloads nothing. A dispatcher
   entry must run the full prologue; skip-prologue stays exclusive to the
   native-chain `JX` where those registers really are live. (Symptom: guest
   loop counters that never converged.)

4. **`jit/{codegen.c,codegen.h,dispatcher.c}` — call8 bridge for windowed
   fast helpers.** The ~30 `HELPER_JIT_*_mmio` helpers are windowed C
   functions; the JIT reached them via a bare `CALLX0`, whose un-paired
   `RETW` corrupted the register window (trashing R_CPU a few calls later).
   Added a one-instruction-style `call8` bridge (`m68k_jit_helper_bridge`,
   raw asm — `naked` is not honored on Xtensa) and route the fast-helper
   path through it (new `ADDR_HELPER_BRIDGE` literal). The host emits the
   unchanged direct call. (Symptom: crash on the first MMIO store.)

5. **`jit/codegen.c` — emit to DRAM scratch, copy to IRAM with 32-bit
   stores.** ESP32-S3 IRAM is 32-bit-access-only; the compiler's branch
   back-patches did byte stores into the executable arena (`LoadStoreError`
   on silicon; qemu doesn't enforce it). Now blocks are emitted into a DRAM
   scratch and word-copied into the code cache. (Symptom: crash compiling
   the first block on hardware.)

6. **`jit/dispatcher.{c,h}` — address-precise SMC invalidation.** Self-
   modifying-code detection was 256-byte-page granular; now it drops only
   blocks whose instruction range actually contains a written byte. (Guards
   against false invalidation from code/data sharing a page — see below it
   wasn't the boot bottleneck, but it's correct and cheap.)

7. **`jit/dispatcher.{c,h}` — hotspot gate (`compile_threshold`).** The
   board's 96 KB IRAM code cache can't hold the System/Finder working set,
   so the boot streamed run-once code through compile→evict→recompile at a
   ~100% miss rate (0.4 MHz). The gate interprets a block until its entry pc
   is seen N (=`BOARD_JIT_HOT_THRESHOLD`, 8) times, only then JITs it — so
   run-once code is interpreted (~1.75 MHz, ~4× faster) and only genuine
   hot loops compile. Off by default (threshold 0) so the host is unchanged.
   A ~2 MB PSRAM counter map (collision-free over the 4 MB RAM pc-space)
   backs it; too small a map saturates and disables the gate.

Validated end-to-end under qemu-system-xtensa (`SELFTEST: PASS`) and on real
PaperS3 hardware (boots System 6.0.5 from SD; cold-boot throughput 0.4 → 1.75
MHz with the gate; hot loops run 8–14 MHz, faster than a real 7.83 MHz Mac
Plus). A residual boot phase whose hot working set exceeds even the 192 KB
IRAM ceiling still recompiles — fundamental to the executable-memory budget,
not a bug.
