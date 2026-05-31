/* FastEPD glue for the M5Stack PaperS3 (third_party/FastEPD).
 *
 * FastEPD is a C++ library; this is the only C++ translation unit in the app.
 * It owns the single FASTEPD instance and exposes plain extern-"C" entry points
 * (see eink_fastepd.h) so the rest of the (C) project can drive the panel.
 *
 * Panel = BB_PANEL_M5PAPERS3 (FastEPD brings up the i80 bus + bit-banged gate +
 * DC/DC boost). The JIT2Holidays fork patches the def's i80 dummy-DC pin from
 * GPIO47 to GPIO49 so it doesn't collide with this board's SD chip-select.
 *
 * Rendering: the guest framebuffer is written DIRECTLY into FastEPD's native
 * 1-bpp buffer (960x540, pitch 120, MSB-first, 1=WHITE) with the same 90°
 * rotation + centered placement the old driver used. Refresh uses FastEPD's
 * native partial(diff)/full updates instead of the old software field engine.
 *
 * Compiled to nothing unless -DBOARD_USE_FASTEPD=1 (the default e-paper driver is
 * the custom epd_panel.c/eink.c). FastEPD stays a component requirement either
 * way so the dependency graph is stable across the toggle; when this TU is empty
 * the linker GC's all FastEPD code out of the image. */

#if defined(BOARD_USE_FASTEPD) && BOARD_USE_FASTEPD

#include "FastEPD.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_rom_crc.h"      /* esp_rom_crc32_le — skip refreshes when fb static */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "eink_fastepd.h"
#include "mac_mem.h"          /* mac_mem / m68k_cpu struct fields */
#include "board_papers3.h"    /* BOARD_EINK_X / BOARD_EINK_Y */
#include <string.h>

static const char *TAG = "fastepd";

/* Native panel geometry (matches the old epd_panel.c img layout exactly). */
#define FE_W       960
#define FE_H       540
#define FE_PITCH   (FE_W / 8)            /* 120 bytes/row */
#define FE_IMG     (FE_PITCH * FE_H)     /* 64800 = the 1bpp image size */
#define MAC_RB     (MAC_SCREEN_W / 8)    /* 64 bytes/guest-row */
/* Guest (gx,gy) -> native (row = BOARD_EINK_X+gx, col = (FE_W-1)-BOARD_EINK_Y-gy),
 * i.e. the same 90°-CCW + centered map as old plot_black (BOARD_EINK_ROT_CW=0). */
#define COL0       ((FE_W - 1) - BOARD_EINK_Y)   /* native col of guest gy=0 */

static FASTEPD       s_epd;
static mac_mem      *s_mem;
static m68k_cpu     *s_cpu;
static const char   *s_engine = "";
static volatile bool s_refresh_req;

void eink_fastepd_request_refresh(void) { s_refresh_req = true; }

/* Scan the guest 512x342 1bpp fb and write it (rotated/centered, colour-inverted
 * — guest 1=black, FastEPD 1=white) into FastEPD's native current buffer. The
 * window starts all-white; only black guest pixels clear their bit. */
static void render_guest(void)
{
    uint8_t *buf = s_epd.currentBuffer();
    memset(buf, 0xFF, FE_IMG);                 /* whole panel white */

    const uint8_t *ram = s_mem->ram;
    const uint32_t rs   = s_mem->ram_size;
    const uint32_t base = s_mem->fb_base;      /* read live: the guest can flip fb */
    for (int gy = 0; gy < MAC_SCREEN_H; gy++) {
        const uint8_t *srow = ram + ((base + (uint32_t)gy * MAC_RB) % rs);
        int col = COL0 - gy;
        int colbyte = col >> 3;
        uint8_t colbit = (uint8_t)(0x80 >> (col & 7));
        for (int bx = 0; bx < MAC_RB; bx++) {
            uint8_t sb = srow[bx];
            if (!sb) continue;                 /* all-white byte: nothing to clear */
            int row0 = BOARD_EINK_X + bx * 8;
            for (int i = 0; i < 8; i++)
                if (sb & (0x80 >> i))
                    buf[(row0 + i) * FE_PITCH + colbyte] &= (uint8_t)~colbit;
        }
    }
}

/* CRC32 of the guest framebuffer (read straight from PSRAM, the only e-ink PSRAM
 * traffic when the screen is static). */
static uint32_t fb_crc(void)
{
    const uint8_t *ram = s_mem->ram;
    const uint32_t rs = s_mem->ram_size, base = s_mem->fb_base;
    uint32_t crc = 0;
    for (int gy = 0; gy < MAC_SCREEN_H; gy++)
        crc = esp_rom_crc32_le(crc, ram + ((base + (uint32_t)gy * MAC_RB) % rs), MAC_RB);
    return crc;
}

static void eink_task(void *arg)
{
    (void)arg;
    /* Baseline: clear to white and full-refresh so partialUpdate has a valid
     * previous plane to diff against. */
    s_epd.fillScreen(BBEP_WHITE);
    s_epd.fullUpdate(CLEAR_SLOW, /*bKeepOn=*/true, NULL);
    ESP_LOGI(TAG, "baseline drawn; entering update loop");

    int64_t last = esp_timer_get_time();
    uint64_t cyc0 = s_cpu->cycles;
    uint32_t frames = 0, drawn = 0;
    int64_t last_partial = 0;
    uint32_t prev_crc = 0;
    for (;;) {
        if (s_refresh_req) {                     /* status-bar tap: full de-ghost */
            s_refresh_req = false;
            s_epd.fillScreen(BBEP_WHITE);
            s_epd.fullUpdate(CLEAR_SLOW, true, NULL);
            prev_crc = 0;                        /* force a redraw of the content */
        }
        uint32_t crc = fb_crc();                 /* ~22 KB PSRAM read; cheap vs a refresh */
        if (crc != prev_crc) {                   /* only refresh when the screen changed */
            prev_crc = crc;
            render_guest();
            int64_t t0 = esp_timer_get_time();
            s_epd.partialUpdate(/*bKeepOn=*/true, 0, FE_H - 1);
            last_partial = esp_timer_get_time() - t0;
            drawn++;
        }
        frames++;

        int64_t now = esp_timer_get_time();
        if (now - last >= 1000000) {
            double mhz = (double)(s_cpu->cycles - cyc0) / (double)(now - last);
            ESP_LOGI(TAG, "polls %u draws %u | partial %lld us | emu %.2f MHz",
                     (unsigned)frames, (unsigned)drawn, (long long)last_partial, mhz);
            last = now; cyc0 = s_cpu->cycles; frames = 0; drawn = 0;
        }
        vTaskDelay(pdMS_TO_TICKS(33));           /* ~30 Hz poll cadence */
    }
}

extern "C" bool eink_fastepd_start(mac_mem *mem, m68k_cpu *cpu, const char *engine)
{
    s_mem = mem; s_cpu = cpu; s_engine = engine ? engine : "";
    int rc = s_epd.initPanel(BB_PANEL_M5PAPERS3, 20000000);
    ESP_LOGI(TAG, "initPanel rc=%d, panel %dx%d", rc, (int)s_epd.width(), (int)s_epd.height());
    if (rc != BBEP_SUCCESS) { ESP_LOGE(TAG, "FastEPD init failed"); return false; }
    s_epd.setPasses(1);   /* 1 partial pass: ~4x faster + ~4x less PSRAM traffic */
    /* Core 1, prio 10 (touch task runs at 11, one above) — same as the old task. */
    xTaskCreatePinnedToCore(eink_task, "eink", 8192, NULL, 10, NULL, 1);
    return true;
}

#endif /* BOARD_USE_FASTEPD */
