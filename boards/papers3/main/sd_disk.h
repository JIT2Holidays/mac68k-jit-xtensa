/* SD-card-backed disk for the Mac emulator's .Sony driver.
 *
 * Mounts the microSD card (FAT32) and exposes a `sony_disk_backend` that
 * streams 512-byte-aligned sector ranges straight off a file on the card,
 * so a multi-hundred-MB / multi-GB disk image is served without ever
 * holding it in RAM. See core/sony.h for the backend contract. */

#ifndef SD_DISK_H
#define SD_DISK_H

#include <stdio.h>
#include "m68k_types.h"
#include "sony.h"

/* Mount the SD card at BOARD_SD_MOUNT. Returns ESP_OK (0) on success. */
int sd_mount(void);
void sd_unmount(void);

/* An open disk-image file on the card, with a single read-ahead cache line.
 * The .Sony driver issues many small (≤1 KB) block reads during boot, mostly
 * sequential; caching a large aligned window turns dozens of per-sector SPI
 * transactions into one, which dominates boot time. */
typedef struct sd_disk {
    FILE *f;
    u32   size;        /* file size in bytes (rounded down to a 512 multiple) */
    bool  wprot;
    /* Read-ahead cache: one aligned window of the image. */
    u8   *cache;       /* SD_DISK_CACHE_BYTES, or NULL = uncached/direct */
    u32   cache_off;   /* image offset of the cached window (aligned)       */
    u32   cache_len;   /* valid bytes in the window                         */
    bool  cache_valid;
} sd_disk;

/* Read-ahead window size; must be a power of two. */
#define SD_DISK_CACHE_BYTES (32u * 1024u)

/* Open `path` as a disk image. `wprot` opens it read-only. Returns true on
 * success; on failure `d->f` is NULL. */
bool sd_disk_open(sd_disk *d, const char *path, bool wprot);
void sd_disk_close(sd_disk *d);

/* Fill `be` with callbacks bound to `d`, ready for sony_attach_backend. */
void sd_disk_backend(const sd_disk *d, sony_disk_backend *be);

/* Cumulative SD-read instrumentation: guest sector-read calls, actual SD
 * transactions (cache misses), bytes read off the card, microseconds spent. */
void sd_disk_stats(uint64_t *calls, uint64_t *txn, uint64_t *bytes, int64_t *us);

#endif /* SD_DISK_H */
