# M5Stack PaperS3 board support

The mac68k-jit-xtensa emulator on the **M5Stack PaperS3** (ESP32-S3, 16 MB
flash, 8 MB octal PSRAM, 4.7" 960×540 e-paper). The **emulator** (68000 +
hard-disk simulation off the SD card) runs on core 0; the **e-ink panel** is
driven on core 1, asynchronously.

## What it does

1. Mounts the microSD card (FAT32) over SPI.
2. Loads `MacPlus.ROM` from the card into RAM and patches the `.Sony`
   driver (so disk I/O is served as logical sectors).
3. Attaches the two disk images as `.Sony` drives backed by **on-demand
   SD reads** — the images are never held in RAM, so a 10 MB System
   volume and a 1 GB Infinite HD work fine on 8 MB of PSRAM.
4. Starts the **e-ink thread on core 1** (see *E-ink display* below).
5. Resets the 68000 and runs it (interpreter by default; JIT optional),
   hot-inserting drive 2 once the desktop is up.

## E-ink display

The panel is an **ED047TC1-class 960×540** module. It is driven **directly**,
the way M5Stack's own firmware and the `~/github/paperboy` reference drive it
(not via a display library): the source driver is fed one row at a time over
the ESP32-S3 **`esp_lcd` i80 bus** (8 data lines + SDCK write clock, SDCE chip
select, latched with SDLE), and the gate (row) driver is **bit-banged** (GDSP
start pulse, GDCK clock). Power is two GPIOs (PWR + the boost-converter enable
BST_EN). There is no PMIC to program and no MCU output-enable (tied on the PCB).

- `main/epd_panel.{c,h}` — the low-level panel: i80 setup, power sequencing,
  the bit-banged gate, and per-row 2-bit drive codes (NOP / drive-black /
  drive-white). Grayscale and anti-ghosting are done in software by sending a
  row across several "fields". **This driver is closed and not included in the
  repo — see [`GET_EPD.md`](GET_EPD.md) for the API contract and how to obtain
  it.** The build needs it present in `main/`.
- `main/eink.{c,h}` — the core-1 drawing thread. **Also closed and not in the
  repo — see [`GET_EPD.md`](GET_EPD.md).**

Boot sequence:

1. **Global refresh** — flash the whole panel black↔white to clear power-on
   garbage and ghosting.
2. **Splash** — decode `msnap.jpg` and show it full-screen as an 8-field
   temporal **gray-scale** full refresh. *Must be a baseline JPEG* — the
   on-device decoder (tjpgd, via `espressif/esp_jpeg`) does not support
   progressive JPEG and falls back to a white screen.
3. **Live view** — only the **view window** (the 512×342 guest screen plus the
   bottom status band) is ever driven; everything outside it keeps showing the
   splash as a background. Each frame, only the pixels that changed since the
   last frame are driven (a fast local refresh); a periodic windowed white
   flash de-ghosts.

Orientation: the device is held in portrait, so content is laid out in a
540×960 "visible" canvas and rotated 90° onto the native-landscape panel
(`BOARD_EINK_ROT_CW` selects the direction). The guest screen sits at
(`BOARD_EINK_X`,`BOARD_EINK_Y`) = (15,98) in that canvas.

Status bar (bottom edge): `INT|JIT  x.xxMHz  y.yFPS` on the left, battery
voltage + `CHG` (when charging) on the right. **FPS is the panel's own refresh
rate** and **MHz is the emulator's effective clock** — both sampled on a
wall-clock timer, independent of each other.

The only managed-component dependency is `espressif/esp_jpeg` (splash decode),
pulled by `main/idf_component.yml`; the i80 driver is ESP-IDF's stock `esp_lcd`.

## SD card layout (FAT32)

```
/MacPlus.ROM
/msnap.jpg                 -> gray-scale boot splash (BASELINE jpeg, ideally <= 540x960)
/disks/System6.0.5.dsk     -> drive 1 (boot volume)
/disks/InfiniteHD6.dsk     -> drive 2 (inserted ~13 s after boot)
```

Copy the ROM/disks from the repo's `roms/` directory onto the card.

## Build & flash

First obtain the closed panel driver (`main/epd_panel.{c,h}`) — see
[`GET_EPD.md`](GET_EPD.md). The build will not link without it. Then:

```sh
source scripts/idf_env.sh          # ESP-IDF v6.0.1
cd boards/papers3
idf.py set-target esp32s3          # once
idf.py build                       # uses the interpreter; add -DBOARD_USE_JIT=1 for the JIT
idf.py -p /dev/ttyACM0 flash monitor
```

The whole emulator core and firmware build at **-O3** (overriding IDF's -O2,
since the interpreter is hot). Console output goes to the native
USB-Serial-JTAG; with `BOARD_DEBUG=1` (default) it prints a periodic heartbeat
(cycles, guest PC, emulated-MHz) plus SD/panel/battery init logs. Build with
`-DBOARD_DEBUG=0` for a quiet release.

## Pin map

All pins live in `main/board_papers3.h` and are overridable at build time
(`-DBOARD_xxx=<gpio>`).

| Function | GPIO(s) |
|----------|---------|
| EPD data D0..D7 | 6, 14, 7, 12, 9, 11, 8, 10 |
| EPD SDCK (i80 WR clock) | 16 |
| EPD SDCE (i80 CS) | 13 |
| EPD SDLE (source latch) | 15 |
| EPD GDCK (gate clock) | 18 |
| EPD GDSP (gate start pulse) | 17 |
| EPD PWR / BST_EN (power, boost) | 45 / 46 |
| microSD (SPI) CS / SCK / MOSI / MISO | 47 / 39 / 38 / 40 |
| Battery voltage (ADC1_CH2, ½× divider) | 3 |
| Charger status (low = charging) | 4 |

EPD bus pins are from M5GFX (`board_M5PaperS3`); PWR/BST_EN are from the PaperS3
schematic; battery/charge pins match M5Unified's PaperS3 power code.

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
| `BOARD_USE_JIT` | `0` | `0` = reference interpreter, `1` = Xtensa JIT (much faster hot loops) |
| `BOARD_DEBUG` | `1` | `1` = verbose heartbeat logging; `0` = quiet release build |
| `BOARD_EINK_ROT_CW` | `0` | Panel rotation: `0` = 90° CCW, `1` = 90° CW (the two differ by 180°) |
| `BOARD_EINK_X` / `BOARD_EINK_Y` | `15` / `98` | Top-left of the 512×342 guest screen in the visible (portrait) canvas |
| `BOARD_BAT_ADC_GPIO` / `BOARD_CHG_STAT_GPIO` | `3` / `4` | Battery ADC pin / charger-status pin |
| `BOARD_RAM_MB` | `4` | Guest RAM (Mac Plus max 4 MB; allocated from PSRAM) |
| `BOARD_JIT_ARENA_KB` | `96` | IRAM JIT code-cache size (JIT only). ~192 KB is the IRAM-EXEC ceiling. |
| `BOARD_JIT_EVICT` | `1` (LRU) | Code-cache eviction: 0=bump, 1=LRU, 2=FIFO, 3=random (JIT only) |
| `BOARD_JIT_HOT_THRESHOLD` | `8` | Hotspot gate: interpret a block until its entry pc is seen this many times, then JIT it (JIT only). |
| `BOARD_HD_INSERT_CYCLE` | `100M` | Cycles after reset to insert drive 2 |

Note: a stale `BOARD_USE_JIT` can linger in the CMake cache (`build/`) and
override the source default — pass `-DBOARD_USE_JIT=…` explicitly, or delete
`build/`, to be sure which engine you get.

## Files

```
boards/papers3/
  CMakeLists.txt            top-level IDF project (applies -O3 to main)
  GET_EPD.md                how to obtain the closed epd_panel.{c,h} driver
  sdkconfig.defaults        PSRAM, 240 MHz, FAT-LFN, watchdogs off, memprot off
  partitions.csv            4 MB factory app (16 MB flash)
  main/
    app_main.c              SD mount -> ROM load -> attach disks -> eink -> run loop
    sd_disk.{c,h}           FAT32 mount + SD-streaming sony_disk_backend
    board_papers3.h         SD + e-ink + battery pin maps, layout, blob paths
    eink.{c,h}              core-1 e-ink thread — NOT in repo, see GET_EPD.md
    epd_panel.{c,h}         low-level ED047TC1 driver — NOT in repo, see GET_EPD.md
    battery.{c,h}           battery voltage (ADC) + charge status (GPIO)
    touch.{c,h}             GT911 capacitive touch -> Mac mouse (track-pad + button circle)
    uart_ctl.{c,h}          USB-Serial-JTAG remote control (screen dump + input)
    idf_component.yml       managed dep: espressif/esp_jpeg (splash decode)
  components/mac68k/
    CMakeLists.txt          full Mac core + JIT (references repo core/ & jit/), -O3
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
