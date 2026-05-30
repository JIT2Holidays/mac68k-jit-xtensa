/* M5Stack PaperS3 board definitions.
 *
 * The PaperS3 is an ESP32-S3 (16 MB flash, 8 MB octal PSRAM) with a 4.7"
 * e-ink panel and a microSD slot. This file pins down the board-specific
 * wiring the firmware needs; the emulator core is board-agnostic.
 *
 * The SD pin map below is the confirmed PaperS3 microSD (SPI) wiring.
 * Overridable from the build (e.g. -DBOARD_SD_CLK=12) or here. The display
 * is intentionally left undefined: this build runs headless. */

#ifndef BOARD_PAPERS3_H
#define BOARD_PAPERS3_H

/* --- microSD (SPI mode) ------------------------------------------------ */
#ifndef BOARD_SD_HOST_SPI        /* 1 = SPI mode (default), see sd_disk.c */
#define BOARD_SD_HOST_SPI 1
#endif

/* SPI bus pins for the TF/microSD slot. */
#ifndef BOARD_SD_CLK
#define BOARD_SD_CLK   39
#endif
#ifndef BOARD_SD_MOSI
#define BOARD_SD_MOSI  38
#endif
#ifndef BOARD_SD_MISO
#define BOARD_SD_MISO  40
#endif
#ifndef BOARD_SD_CS
#define BOARD_SD_CS    47
#endif

/* FATFS mount point for the SD card. */
#ifndef BOARD_SD_MOUNT
#define BOARD_SD_MOUNT "/sdcard"
#endif

/* --- e-ink panel (ED047TC1-class, 960x540) ----------------------------
 * Driven the way M5Stack's own firmware drives the PaperS3 panel (and as the
 * "paperboy" reference does): the source driver is fed one row at a time over
 * the ESP32-S3 LCD i80 bus (8-bit data + SDCK as the write clock, SDCE as CS),
 * latched with SDLE; the gate driver is bit-banged (GDSP start pulse, GDCK
 * clock). Two plain GPIOs gate power: PWR and the boost-converter enable
 * BST_EN. There is no MCU output-enable and no PMU GPIO. Pins are from the
 * PaperS3 schematic. Override any with -DBOARD_EPD_xxx=<gpio>.               */
#ifndef BOARD_EPD_D0
#define BOARD_EPD_D0   6
#endif
#ifndef BOARD_EPD_D1
#define BOARD_EPD_D1   14
#endif
#ifndef BOARD_EPD_D2
#define BOARD_EPD_D2   7
#endif
#ifndef BOARD_EPD_D3
#define BOARD_EPD_D3   12
#endif
#ifndef BOARD_EPD_D4
#define BOARD_EPD_D4   9
#endif
#ifndef BOARD_EPD_D5
#define BOARD_EPD_D5   11
#endif
#ifndef BOARD_EPD_D6
#define BOARD_EPD_D6   8
#endif
#ifndef BOARD_EPD_D7
#define BOARD_EPD_D7   10
#endif
#ifndef BOARD_EPD_SDCK      /* source-driver clock = LCD i80 write clock (WR) */
#define BOARD_EPD_SDCK 16
#endif
#ifndef BOARD_EPD_SDCE      /* source-driver chip-enable = i80 CS */
#define BOARD_EPD_SDCE 13
#endif
#ifndef BOARD_EPD_SDLE      /* source-driver latch enable (bit-banged) */
#define BOARD_EPD_SDLE 15
#endif
#ifndef BOARD_EPD_GDCK      /* gate-driver clock (bit-banged) */
#define BOARD_EPD_GDCK 18
#endif
#ifndef BOARD_EPD_GDSP      /* gate-driver start pulse (bit-banged) */
#define BOARD_EPD_GDSP 17
#endif
#ifndef BOARD_EPD_PWR       /* panel power enable (schematic: PWR) */
#define BOARD_EPD_PWR  45
#endif
#ifndef BOARD_EPD_BST       /* boost-converter enable (schematic: BST_EN) */
#define BOARD_EPD_BST  46
#endif
/* LCD i80 pixel clock (Hz) and the per-row trailing pad (source-driver
 * shift-register flush), matching the reference driver. */
#ifndef BOARD_EPD_PCLK_HZ
#define BOARD_EPD_PCLK_HZ  28000000
#endif
#ifndef BOARD_EPD_LINE_PAD
#define BOARD_EPD_LINE_PAD 16
#endif

/* msnap.jpg — the boot splash, shown full-screen in gray scale at startup. */
#ifndef BOARD_PATH_SNAP
#define BOARD_PATH_SNAP   BOARD_SD_MOUNT "/msnap.jpg"
#endif

/* Where the 512x342 guest screen lands on the panel, in landscape pixels with
 * (0,0) at the top-left corner: the framebuffer is rendered at (15,98) to
 * (526,439) inclusive (= 512x342). */
#ifndef BOARD_EINK_X
#define BOARD_EINK_X   15
#endif
#ifndef BOARD_EINK_Y
#define BOARD_EINK_Y   98
#endif

/* --- touch (GT911 capacitive) -> Macintosh mouse ----------------------
 * The whole panel is a relative touch-pad; a circular zone in the bottom-left
 * of the visible (540x960) canvas, marked on screen by a dotted outline, is
 * the mouse button. GT911 is on its own I2C bus. */
#ifndef BOARD_TOUCH_SDA
#define BOARD_TOUCH_SDA 41
#endif
#ifndef BOARD_TOUCH_SCL
#define BOARD_TOUCH_SCL 42
#endif
#ifndef BOARD_TOUCH_INT
#define BOARD_TOUCH_INT 48     /* active-low when a touch sample is ready */
#endif
/* Raw GT911 (x:0..539, y:0..959) -> visible-canvas axis flips. The GT911 native
 * space matches the 540x960 canvas; flip per-axis if movement / the button zone
 * come out mirrored on hardware. */
#ifndef BOARD_TOUCH_FLIP_X
#define BOARD_TOUCH_FLIP_X 1
#endif
#ifndef BOARD_TOUCH_FLIP_Y
#define BOARD_TOUCH_FLIP_Y 1
#endif
/* Track-pad sensitivity: Mac-cursor pixels per touch pixel, ×10 (15 = 1.5×). */
#ifndef BOARD_TOUCH_SENS_X10
#define BOARD_TOUCH_SENS_X10 15
#endif
/* Mouse-button circle in the visible canvas (bottom-left, just above the
 * status bar). Tangent to the left edge by default. */
#ifndef BOARD_BTN_R
#define BOARD_BTN_R  70
#endif
#ifndef BOARD_BTN_CX
#define BOARD_BTN_CX BOARD_BTN_R
#endif
#ifndef BOARD_BTN_CY
#define BOARD_BTN_CY (960 - 28 - BOARD_BTN_R)
#endif

/* --- blob layout on the SD card (FAT32) -------------------------------
 * The ROM is embedded in flash (see main/CMakeLists.txt); only the disk
 * images live on the card:
 *   /disks/System6.0.5.dsk      (boot volume, drive 1)
 *   /disks/InfiniteHD6.dsk      (drive 2, inserted after boot)            */
#define BOARD_PATH_BOOT   BOARD_SD_MOUNT "/disks/System6.0.5.dsk"
#define BOARD_PATH_HD     BOARD_SD_MOUNT "/disks/InfiniteHD6.dsk"

#endif /* BOARD_PAPERS3_H */
