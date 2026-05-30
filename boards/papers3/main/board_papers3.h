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

/* --- blob layout on the SD card (FAT32) -------------------------------
 * The ROM is embedded in flash (see main/CMakeLists.txt); only the disk
 * images live on the card:
 *   /disks/System6.0.5.dsk      (boot volume, drive 1)
 *   /disks/InfiniteHD6.dsk      (drive 2, inserted after boot)            */
#define BOARD_PATH_BOOT   BOARD_SD_MOUNT "/disks/System6.0.5.dsk"
#define BOARD_PATH_HD     BOARD_SD_MOUNT "/disks/InfiniteHD6.dsk"

#endif /* BOARD_PAPERS3_H */
