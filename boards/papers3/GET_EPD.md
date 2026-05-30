# Obtaining the closed e-ink display sources

The e-ink display sources for the PaperS3 are **not included in this
repository** — they are derived from a closed/proprietary PaperS3 panel
reference, so they are kept out of source control (see `.gitignore`) and must
be supplied separately before the firmware will build or run:

| File | Role |
|------|------|
| `main/epd_panel.{c,h}` | low-level ED047TC1 driver (esp_lcd i80 + bit-banged gate) |
| `main/eink.{c,h}` | the core-1 drawing thread (splash, windowed view, status bar, 1-field/frame driver) |

Everything else (battery sensing, the touch→mouse driver, the board config,
the emulator) is here; only the four files above are withheld. `app_main.c`
calls into `eink.h`, and `eink.c` calls into `epd_panel.h`, so all four are
needed to link.

## How to obtain it

Pick whichever applies to you:

- **You have access to the reference.** Port it to the API contract below
  (it is a thin wrapper: i80 source bus + a bit-banged gate driver). The
  M5Stack PaperS3 panel is driven exactly the way M5Stack's own firmware and
  the Modos "Smooth Graphics" / `paperboy` reference drive it.
- **You don't.** Contact the maintainer of this branch for the files, or
  implement your own `epd_panel.{c,h}` against the contract below using your
  panel's datasheet. The pin map is already in `main/board_papers3.h`
  (`BOARD_EPD_*`), and the build wires `esp_lcd` for the i80 bus.

Drop the two files into `main/`. They are git-ignored, so they will not be
committed by accident.

## API contract

`eink.c` includes `epd_panel.h` and calls the functions below. Implement these
and the firmware builds unchanged. Drive model: each panel "field" sends every
row once, with a 2-bit code per pixel; driving a pixel across several fields
moves it toward black or white. Grayscale / anti-ghosting is the caller's job
(in `eink.c`), so the driver only needs these primitives.

```c
/* epd_panel.h */
#define EPD_PANEL_W       960   /* native panel width  (landscape) */
#define EPD_PANEL_H       540   /* native panel height (landscape) */

#define EPD_CODE_NOP      0x0   /* leave the pixel unchanged */
#define EPD_CODE_BLACK    0x1   /* drive toward black        */
#define EPD_CODE_WHITE    0x2   /* drive toward white        */

/* Bytes of packed 2-bit drive codes per row (4 pixels/byte). */
#define EPD_ROW_CODE_BYTES (EPD_PANEL_W / 4)   /* 240 */

void      epd_panel_init(void);        /* GPIO + i80 bus + power on; call once */
void      epd_panel_frame_start(void); /* reset the gate driver to the top row */
uint8_t  *epd_panel_row(void);         /* current writable row buffer (>= EPD_ROW_CODE_BYTES) */
void      epd_panel_row_send(void);    /* latch + advance one gate line, DMA the row, flip buffers */
void      epd_panel_frame_end(void);   /* trailing dummy line + wait for DMA to drain */
```

`app_main.c` includes `eink.h` and calls:

```c
/* eink.h */
struct mac_mem; struct m68k_cpu;
/* Launch the core-1 drawing thread (panel init, splash, live view). `engine`
 * is a short label ("JIT"/"INT") shown in the status bar. Blocks until the
 * splash is drawn, then returns; the draw loop keeps running on core 1. */
bool eink_start(struct mac_mem *mem, struct m68k_cpu *cpu, const char *engine);
```

Expected behavior:

- `epd_panel_init()` — configure the source-driver i80 bus (8 data lines +
  write clock + chip-select), the bit-banged gate lines, and the power GPIOs
  (`BOARD_EPD_PWR`, `BOARD_EPD_BST`), then power the panel on. Pins come from
  `board_papers3.h`.
- A full field is: `epd_panel_frame_start()`, then `EPD_PANEL_H` iterations of
  *(fill `epd_panel_row()` with `EPD_ROW_CODE_BYTES` of codes)* +
  `epd_panel_row_send()`, then `epd_panel_frame_end()`.
- Row code packing must match the source-driver order; in the reference, each
  input byte of an MSB-first 1-bpp row expands to two code bytes via a nibble
  lookup (`out[2b] = lut[hi_nibble]`, `out[2b+1] = lut[lo_nibble]`), with the
  black/white codes placed per the table above. `eink.c` assumes this exact
  packing when it builds rows.

## Pin map (from `board_papers3.h`)

| Function | GPIO(s) |
|----------|---------|
| Data D0..D7 | 6, 14, 7, 12, 9, 11, 8, 10 |
| SDCK (i80 write clock) | 16 |
| SDCE (i80 chip-select) | 13 |
| SDLE (source latch) | 15 |
| GDCK (gate clock) | 18 |
| GDSP (gate start pulse) | 17 |
| PWR / BST_EN (power, boost) | 45 / 46 |

i80 pixel clock and the per-row trailing pad are `BOARD_EPD_PCLK_HZ`
(28 MHz) and `BOARD_EPD_LINE_PAD` (16) in `board_papers3.h`.
