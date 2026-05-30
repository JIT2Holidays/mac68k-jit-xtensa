/* SD-card-backed .Sony disk backend for the PaperS3. */

#include "sd_disk.h"
#include "board_papers3.h"

#include <string.h>
#include <stdlib.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"

static const char *TAG = "sd";
static sdmmc_card_t *s_card;

/* Instrumentation: guest sector-read calls vs actual SD transactions, bytes
 * read off the card, and wall-time spent in those reads. */
static struct { uint64_t calls, txn, bytes; int64_t us; } s_stat;
void sd_disk_stats(uint64_t *calls, uint64_t *txn, uint64_t *bytes, int64_t *us) {
    *calls = s_stat.calls; *txn = s_stat.txn; *bytes = s_stat.bytes; *us = s_stat.us;
}

/* Timed fseek+fread into `dst`; updates the SD transaction counters. */
static bool sd_pread(sd_disk *d, u32 off, void *dst, u32 n) {
    int64_t t = esp_timer_get_time();
    bool ok = (fseek(d->f, (long)off, SEEK_SET) == 0) &&
              (fread(dst, 1, n, d->f) == n);
    s_stat.us += esp_timer_get_time() - t;
    s_stat.txn++;
    s_stat.bytes += n;
    return ok;
}

int sd_mount(void) {
    esp_vfs_fat_sdmmc_mount_config_t mcfg = {
        .format_if_mount_failed = false,
        .max_files = 4,
        .allocation_unit_size = 16 * 1024,
    };

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    spi_bus_config_t bus = {
        .mosi_io_num = BOARD_SD_MOSI,
        .miso_io_num = BOARD_SD_MISO,
        .sclk_io_num = BOARD_SD_CLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4096,
    };
    esp_err_t err = spi_bus_initialize(host.slot, &bus, SDSPI_DEFAULT_DMA);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_initialize failed: %s", esp_err_to_name(err));
        return err;
    }

    sdspi_device_config_t dev = SDSPI_DEVICE_CONFIG_DEFAULT();
    dev.gpio_cs = BOARD_SD_CS;
    dev.host_id = host.slot;

    err = esp_vfs_fat_sdspi_mount(BOARD_SD_MOUNT, &host, &dev, &mcfg, &s_card);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mount %s failed: %s (check SD wiring / FAT32 format)",
                 BOARD_SD_MOUNT, esp_err_to_name(err));
        spi_bus_free(host.slot);
        return err;
    }
    ESP_LOGI(TAG, "mounted %s", BOARD_SD_MOUNT);
    sdmmc_card_print_info(stdout, s_card);
    return ESP_OK;
}

void sd_unmount(void) {
    if (s_card) {
        esp_vfs_fat_sdcard_unmount(BOARD_SD_MOUNT, s_card);
        s_card = NULL;
    }
}

bool sd_disk_open(sd_disk *d, const char *path, bool wprot) {
    memset(d, 0, sizeof(*d));
    d->f = fopen(path, wprot ? "rb" : "r+b");
    if (!d->f) {
        ESP_LOGE(TAG, "open %s failed", path);
        return false;
    }
    if (fseek(d->f, 0, SEEK_END) != 0) { fclose(d->f); d->f = NULL; return false; }
    long sz = ftell(d->f);
    if (sz <= 0) { fclose(d->f); d->f = NULL; return false; }
    d->size  = (u32)(sz & ~0x1FFL);   /* whole 512-byte blocks only */
    d->wprot = wprot;
    /* Unbuffered: we do our own block-granular fseek/fread plus the
     * read-ahead cache below; stdio's buffer would just add a redundant
     * copy on every sector. */
    setvbuf(d->f, NULL, _IONBF, 0);
    /* Read-ahead window (PSRAM via the >4 KB malloc threshold). A failed
     * alloc is non-fatal — the backend falls back to direct per-read I/O. */
    d->cache = (u8 *)malloc(SD_DISK_CACHE_BYTES);
    d->cache_valid = false;
    ESP_LOGI(TAG, "opened %s: %u bytes (%u blocks)%s%s",
             path, d->size, d->size >> 9, wprot ? " [ro]" : "",
             d->cache ? "" : " [no cache]");
    return true;
}

void sd_disk_close(sd_disk *d) {
    if (d->cache) { free(d->cache); d->cache = NULL; }
    if (d->f) { fclose(d->f); d->f = NULL; }
}

/* Load the aligned window containing `off` into the cache. Returns false on
 * an I/O error (caller then falls back to a direct read). */
static bool cache_fill(sd_disk *d, u32 line_off) {
    u32 want = SD_DISK_CACHE_BYTES;
    if (line_off + want > d->size) want = d->size - line_off;
    if (!sd_pread(d, line_off, d->cache, want)) {
        d->cache_valid = false;
        return false;
    }
    d->cache_off = line_off;
    d->cache_len = want;
    d->cache_valid = true;
    return true;
}

static bool be_read(void *ctx, u32 off, u8 *dst, u32 len) {
    sd_disk *d = (sd_disk *)ctx;
    s_stat.calls++;
    if (!d->cache) {                          /* uncached: direct */
        return sd_pread(d, off, dst, len);
    }
    while (len) {
        bool hit = d->cache_valid && off >= d->cache_off &&
                   off < d->cache_off + d->cache_len;
        if (!hit) {
            if (!cache_fill(d, off & ~(SD_DISK_CACHE_BYTES - 1u))) {
                /* I/O error filling the window — serve this read directly. */
                return sd_pread(d, off, dst, len);
            }
        }
        u32 avail = d->cache_off + d->cache_len - off;
        u32 n = len < avail ? len : avail;
        memcpy(dst, d->cache + (off - d->cache_off), n);
        dst += n; off += n; len -= n;
    }
    return true;
}

static bool be_write(void *ctx, u32 off, const u8 *src, u32 len) {
    sd_disk *d = (sd_disk *)ctx;
    if (fseek(d->f, (long)off, SEEK_SET) != 0) return false;
    if (fwrite(src, 1, len, d->f) != len) return false;
    fflush(d->f);
    /* Keep the read-ahead window coherent: invalidate it if the write
     * touched any byte it holds. */
    if (d->cache_valid && off < d->cache_off + d->cache_len &&
        off + len > d->cache_off) {
        d->cache_valid = false;
    }
    return true;
}

void sd_disk_backend(const sd_disk *d, sony_disk_backend *be) {
    memset(be, 0, sizeof(*be));
    be->read  = be_read;
    be->write = d->wprot ? NULL : be_write;
    be->ctx   = (void *)d;
    be->size  = d->size;
    be->wprot = d->wprot;
}
