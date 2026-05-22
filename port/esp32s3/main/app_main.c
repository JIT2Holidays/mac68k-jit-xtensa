/* mac68k-jit-xtensa — ESP32-S3 firmware.
 *
 * Runs the built-in 68000 demo program under both the reference
 * interpreter and the Xtensa JIT, on a real ESP32-S3 core (here, under
 * qemu-system-xtensa). The demo itself prints "RESULT: PASS" over the
 * UART when it completes successfully; this harness prints a parseable
 * [BENCH] line per mode and a final summary.
 *
 * This is the "CPU part running in qemu-xtensa" deliverable: the 68000
 * guest is translated to Xtensa machine code at runtime and executed
 * natively by the emulated LX7 core. */

#include "m68k_cpu.h"
#include "m68k_interp.h"
#include "mac_mem.h"
#include "demo_rom.h"
#include "dispatcher.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <inttypes.h>

static const char *TAG = "mac68k";

#define MAC_RAM_BYTES   (128u * 1024u)
#define CYCLE_BUDGET    (100ull * 1000ull * 1000ull)

static u8       s_image[DEMO_ROM_MAX];
static mac_mem  s_mem;
static m68k_cpu s_cpu;

/* The guest's debug serial port routes straight to the UART. */
static void serial_sink(void *ctx, u8 b) {
    (void)ctx;
    putchar((int)b);
}

static u32 fb_checksum(const mac_mem *m) {
    u32 sum = 0;
    for (u32 i = 0; i < 512; i++)
        sum = (sum * 31u) + mac_read8((mac_mem *)m, m->fb_base + i);
    return sum;
}

/* Run the demo once. Returns the guest exit code (0 = PASS). */
static int run_interp(void) {
    mac_mem_init(&s_mem, MAC_RAM_BYTES);
    s_mem.serial_sink = serial_sink;
    u32 len = demo_rom_build(s_image, s_mem.fb_base);
    mac_load_ram_image(&s_mem, 0, s_image, len);
    m68k_reset(&s_cpu, &s_mem);

    int64_t t0 = esp_timer_get_time();
    m68k_run_until(&s_cpu, CYCLE_BUDGET);
    int64_t us = esp_timer_get_time() - t0;

    double mhz = us > 0 ? (double)s_cpu.cycles / (double)us : 0.0;
    printf("[BENCH] mode=interp cycles=%" PRIu64 " elapsed_us=%" PRId64
           " mhz=%.3f pc=0x%06X exit=%d fbsum=0x%08X\n",
           s_cpu.cycles, us, mhz, (unsigned)s_cpu.pc, s_cpu.exit_code,
           (unsigned)fb_checksum(&s_mem));
    int rc = s_cpu.exit_code;
    mac_mem_free(&s_mem);
    return rc;
}

static int run_jit(void) {
    mac_mem_init(&s_mem, MAC_RAM_BYTES);
    s_mem.serial_sink = serial_sink;
    u32 len = demo_rom_build(s_image, s_mem.fb_base);
    mac_load_ram_image(&s_mem, 0, s_image, len);
    m68k_reset(&s_cpu, &s_mem);

    m68k_dispatcher d;
    if (!m68k_dispatcher_init(&d, &s_cpu)) {
        ESP_LOGE(TAG, "JIT dispatcher init failed (codecache arena alloc)");
        mac_mem_free(&s_mem);
        return -1;
    }

    int64_t t0 = esp_timer_get_time();
    m68k_dispatcher_run_until(&d, CYCLE_BUDGET);
    int64_t us = esp_timer_get_time() - t0;

    double mhz = us > 0 ? (double)s_cpu.cycles / (double)us : 0.0;
    printf("[BENCH] mode=jit cycles=%" PRIu64 " elapsed_us=%" PRId64
           " mhz=%.3f pc=0x%06X exit=%d fbsum=0x%08X arena_kb=%u "
           "blocks=%" PRIu64 "/%" PRIu64 " inline_ops=%" PRIu64
           " helper_ops=%" PRIu64 " chain=%" PRIu64 "/%" PRIu64 "\n",
           s_cpu.cycles, us, mhz, (unsigned)s_cpu.pc, s_cpu.exit_code,
           (unsigned)fb_checksum(&s_mem), (unsigned)(d.arena_cap / 1024u),
           d.blocks_compiled, d.blocks_executed, d.inline_ops_total,
           d.helper_ops_total, d.chain_hits, d.chain_misses);
    int rc = s_cpu.exit_code;
    m68k_dispatcher_shutdown(&d);
    mac_mem_free(&s_mem);
    return rc;
}

void app_main(void) {
    ESP_LOGI(TAG, "boot — mac68k-jit-xtensa (68000 Macintosh CPU on Xtensa LX7)");
    ESP_LOGI(TAG, "guest RAM=%uKB", (unsigned)(MAC_RAM_BYTES / 1024));

    int ri = run_interp();
    int rj = run_jit();

    if (ri == 0 && rj == 0) {
        printf("RESULT: PASS\n");
    } else {
        printf("RESULT: FAIL (interp=%d jit=%d)\n", ri, rj);
    }
    fflush(stdout);

    /* Park forever — qemu's instrumentation flush is happiest if we don't
     * unwind through the windowed call chain after this point. */
    while (1) vTaskDelay(portMAX_DELAY);
}
