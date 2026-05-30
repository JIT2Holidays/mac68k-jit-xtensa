/* mac68k-jit-xtensa — M5Stack PaperS3 firmware.
 *
 * Boots a Macintosh Plus on the ESP32-S3: the 68000 guest runs under the
 * Xtensa JIT, the ROM is loaded from the SD card, and the hard disks are
 * served on-demand straight off the FAT32 card (no in-RAM image copy) via
 * the .Sony streaming backend. The display is intentionally NOT driven
 * here — this is the CPU-core + HDD-simulation bring-up; the e-ink panel
 * code lands later. The framebuffer still lives in guest RAM, so a display
 * driver added later only has to scan it out.
 *
 * SD card layout (FAT32):
 *   /MacPlus.ROM
 *   /disks/System6.0.5.dsk   -> drive 1 (boot volume)
 *   /disks/InfiniteHD6.dsk   -> drive 2 (inserted after boot)
 */

#include "m68k_cpu.h"
#include "m68k_interp.h"
#include "mac_mem.h"
#include "sony.h"
#include "dispatcher.h"
#include "codecache.h"
#include "demo_rom.h"

#include "board_papers3.h"
#include "sd_disk.h"
#include "uart_ctl.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <inttypes.h>
#include <stdio.h>

static const char *TAG = "mac68k";

/* The Mac Plus ROM, embedded in flash (see main/CMakeLists.txt EMBED_FILES).
 * mac_load_rom() copies it into RAM and patches the .Sony driver there, so
 * the read-only flash copy is never modified. */
extern const u8 macplus_rom_start[] asm("_binary_MacPlus_ROM_start");
extern const u8 macplus_rom_end[]   asm("_binary_MacPlus_ROM_end");

/* --- tunables ---------------------------------------------------------- */

/* Run the JIT (1) or the reference interpreter (0). The JIT is the point
 * of this project; the interpreter is the slower, always-correct fallback
 * for first bring-up on new silicon. */
#ifndef BOARD_USE_JIT
#define BOARD_USE_JIT 1
#endif

/* Build the SD-less interp-vs-JIT parity self-test instead of the real
 * boot. Used to validate the Xtensa JIT under qemu (see scripts/). */
#ifndef BOARD_QEMU_SELFTEST
#define BOARD_QEMU_SELFTEST 0
#endif

/* Lock-step debug mode: boot two machines (interpreter + JIT) in parallel
 * from reset and report the first block where their CPU state diverges —
 * to locate the native-Xtensa JIT bug that the host can't see (the host
 * runs the JIT through the software sim). Uses 1 MB RAM per machine. */
#ifndef BOARD_LOCKSTEP
#define BOARD_LOCKSTEP 0
#endif
/* Cycle from which lock-step also does the (expensive) full-RAM compare to
 * catch a mis-stored value before it shows up in registers. */
#ifndef BOARD_LS_RAMCMP_FROM
#define BOARD_LS_RAMCMP_FROM 0xFFFFFFFFFFull   /* off by default (set low to enable) */
#endif

/* Guest RAM. The Mac Plus tops out at 4 MB; it comes from PSRAM (see
 * sdkconfig.defaults — SPIRAM_USE_MALLOC with a low internal threshold). */
#ifndef BOARD_RAM_MB
#define BOARD_RAM_MB 4u
#endif

/* JIT code-cache arena (IRAM, executable). Far smaller than the host's
 * 1 MB default — IRAM is scarce — so we run with LRU eviction and let cold
 * blocks recompile. Override with -DBOARD_JIT_ARENA_KB=<n>. */
#ifndef BOARD_JIT_ARENA_KB
#define BOARD_JIT_ARENA_KB 96u
#endif

/* Code-cache eviction policy: CC_MODE_BUMP=0, CC_MODE_LRU=1, CC_MODE_FIFO=2,
 * CC_MODE_RANDOM=3. The System working set exceeds the IRAM arena, so the
 * policy (and its per-eviction cost) drives boot throughput. */
#ifndef BOARD_JIT_EVICT
#define BOARD_JIT_EVICT CC_MODE_LRU
#endif

/* Hotspot gate: interpret a block until its entry pc has been seen this
 * many times, then JIT it. Avoids compiling the flood of run-once code an
 * OS streams through at boot (which thrashes the IRAM-bounded code cache).
 * 0 = compile on first sight. */
#ifndef BOARD_JIT_HOT_THRESHOLD
#define BOARD_JIT_HOT_THRESHOLD 8u
#endif

/* When to insert drive 2 (the Infinite HD), in guest cycles after reset.
 * A disk present at reset mounts at the .Sony level, but the Finder isn't
 * running yet to show its icon; inserting it ~13 s in (at 7.83 MHz) lands
 * a real disk-inserted event once the desktop is up. */
#ifndef BOARD_HD_INSERT_CYCLE
#define BOARD_HD_INSERT_CYCLE (100ull * 1000 * 1000)
#endif

/* --- emulator state ---------------------------------------------------- */

static mac_mem        s_mem;
static m68k_cpu       s_cpu;
static m68k_dispatcher s_disp;
#if !BOARD_QEMU_SELFTEST
static sd_disk        s_boot;     /* drive 1 */
static sd_disk        s_hd;       /* drive 2 */
static bool           s_hd_open;
#endif

/* The guest's debug serial port goes to the UART console. */
static void serial_sink(void *ctx, u8 b) { (void)ctx; putchar((int)b); }

/* --- qemu self-test ---------------------------------------------------
 * Built when -DBOARD_QEMU_SELFTEST=1. Runs the built-in 68000 demo under
 * the interpreter and the JIT and checks they reach an identical final
 * state — the ground-truth test that the Xtensa JIT (native block entry +
 * chaining + I-cache coherency) is correct on real LX7 silicon, with no
 * SD card required. Prints "RESULT: PASS" iff both engines agree. */
#if BOARD_QEMU_SELFTEST
#define SELFTEST_RAM_BYTES (128u * 1024u)
#define SELFTEST_BUDGET    (100ull * 1000 * 1000)

static u32 fb_checksum(const mac_mem *m) {
    u32 sum = 0;
    for (u32 i = 0; i < 512; i++)
        sum = (sum * 31u) + mac_read8((mac_mem *)m, m->fb_base + i);
    return sum;
}

static int selftest_run(bool use_jit, u32 *fbsum_out) {
    static u8 image[DEMO_ROM_MAX];
    mac_mem_init(&s_mem, SELFTEST_RAM_BYTES);
    s_mem.serial_sink = serial_sink;
    u32 len = demo_rom_build(image, s_mem.fb_base);
    mac_load_ram_image(&s_mem, 0, image, len);
    m68k_reset(&s_cpu, &s_mem);

    int64_t t0 = esp_timer_get_time();
    if (use_jit) {
        if (!m68k_dispatcher_init_ex(&s_disp, &s_cpu,
                                     BOARD_JIT_ARENA_KB, CC_MODE_LRU)) {
            ESP_LOGE(TAG, "selftest: JIT init failed");
            mac_mem_free(&s_mem);
            return -1;
        }
        m68k_dispatcher_run_until(&s_disp, SELFTEST_BUDGET);
    } else {
        m68k_run_until(&s_cpu, SELFTEST_BUDGET);
    }
    int64_t us = esp_timer_get_time() - t0;
    u32 fbsum = fb_checksum(&s_mem);
    double mhz = us > 0 ? (double)s_cpu.cycles / (double)us : 0.0;
    ESP_LOGI(TAG, "[BENCH] mode=%s cycles=%llu mhz=%.3f pc=0x%06" PRIX32
             " exit=%d fbsum=0x%08" PRIX32, use_jit ? "jit" : "interp",
             (unsigned long long)s_cpu.cycles, mhz, s_cpu.pc,
             s_cpu.exit_code, fbsum);
    int rc = s_cpu.exit_code;
    if (use_jit) {
        ESP_LOGI(TAG, "[JIT] blocks=%llu/%llu chain=%llu/%llu fallbacks=%llu "
                 "resets=%llu smc=%llu halted=%d",
                 (unsigned long long)s_disp.blocks_compiled,
                 (unsigned long long)s_disp.blocks_executed,
                 (unsigned long long)s_disp.chain_hits,
                 (unsigned long long)s_disp.chain_misses,
                 (unsigned long long)s_disp.interp_fallbacks,
                 (unsigned long long)s_disp.arena_resets,
                 (unsigned long long)s_disp.smc_invalidations, s_cpu.halted);
        m68k_dispatcher_shutdown(&s_disp);
    }
    if (fbsum_out) *fbsum_out = fbsum;
    mac_mem_free(&s_mem);
    return rc;
}

/* Run a short fixed budget and dump the registers that the demo's opening
 * sum-loop touches, under each engine, to localize the first divergence. */
static void selftest_diverge(void) {
    static u8 image[DEMO_ROM_MAX];
    const u64 budgets[] = { 20, 28, 36, 44 };
    for (int jit = 0; jit < 2; jit++) {
        for (unsigned bi = 0; bi < sizeof(budgets)/sizeof(budgets[0]); bi++) {
            mac_mem_init(&s_mem, SELFTEST_RAM_BYTES);
            s_mem.serial_sink = NULL;
            u32 len = demo_rom_build(image, s_mem.fb_base);
            mac_load_ram_image(&s_mem, 0, image, len);
            m68k_reset(&s_cpu, &s_mem);
            if (jit) {
                m68k_dispatcher_init_ex(&s_disp, &s_cpu, BOARD_JIT_ARENA_KB, CC_MODE_LRU);
                m68k_dispatcher_run_until(&s_disp, budgets[bi]);
            } else {
                m68k_run_until(&s_cpu, budgets[bi]);
            }
            ESP_LOGI(TAG, "[DIV %s b=%llu] cyc=%llu pc=0x%06" PRIX32
                     " sr=0x%04X D0=%08" PRIX32 " D1=%08" PRIX32,
                     jit ? "jit" : "int", (unsigned long long)budgets[bi],
                     (unsigned long long)s_cpu.cycles, s_cpu.pc, s_cpu.sr,
                     s_cpu.d[0], s_cpu.d[1]);
            if (jit) m68k_dispatcher_shutdown(&s_disp);
            mac_mem_free(&s_mem);
        }
    }
}

static void selftest_main(void) {
    ESP_LOGI(TAG, "JIT self-test (interp vs JIT on the built-in demo)");
    /* Set to 1 to dump per-budget interp-vs-JIT register divergence (note:
     * leaks the IRAM JIT arena across its repeated dispatcher inits). */
    if (0) selftest_diverge();
    u32 fbi = 0, fbj = 0;
    int ri = selftest_run(false, &fbi);
    int rj = selftest_run(true,  &fbj);
    /* Distinct prefix from the guest demo's own "RESULT: PASS" serial. */
    if (ri == 0 && rj == 0 && fbi == fbj) {
        printf("SELFTEST: PASS\n");
    } else {
        printf("SELFTEST: FAIL (interp=%d jit=%d fbsum interp=0x%08" PRIX32
               " jit=0x%08" PRIX32 ")\n", ri, rj, fbi, fbj);
    }
    fflush(stdout);
}
#endif /* BOARD_QEMU_SELFTEST */

#if !BOARD_QEMU_SELFTEST
static bool attach_disk(sd_disk *d, int drive, const char *path, bool wprot) {
    if (!sd_disk_open(d, path, wprot)) return false;
    sony_disk_backend be;
    sd_disk_backend(d, &be);
    if (!sony_attach_backend(&s_mem, drive, &be)) {
        ESP_LOGE(TAG, "sony_attach_backend(drive %d) failed", drive);
        sd_disk_close(d);
        return false;
    }
    ESP_LOGI(TAG, "drive %d <- %s", drive + 1, path);
    return true;
}
#endif /* !BOARD_QEMU_SELFTEST */

#if BOARD_LOCKSTEP
/* Two parallel machines sharing the global .Sony driver (re-pointed per
 * engine with sony_set_vm, mirroring the host --diff-jit-trace). */
static mac_mem        ls_mi, ls_mj;
static m68k_cpu       ls_ci, ls_cj;
static m68k_dispatcher ls_dj;
static sd_disk        ls_disk;

static void lockstep_main(void) {
    ESP_LOGI(TAG, "LOCK-STEP: interp vs JIT from reset (1 MB each)");
    if (sd_mount() != 0) { ESP_LOGE(TAG, "no SD"); return; }
    mac_mem_init(&ls_mi, 1u << 20);
    mac_mem_init(&ls_mj, 1u << 20);
    if (!ls_mi.ram || !ls_mj.ram) { ESP_LOGE(TAG, "RAM alloc failed"); return; }
    u32 rlen = (u32)(macplus_rom_end - macplus_rom_start);
    mac_load_rom(&ls_mi, macplus_rom_start, rlen);
    mac_load_rom(&ls_mj, macplus_rom_start, rlen);
    /* Boot disk into the shared .Sony driver (both engines read it via
     * sony_set_vm; the SD backend streams the same file for both). */
    if (!sd_disk_open(&ls_disk, BOARD_PATH_BOOT, false)) { ESP_LOGE(TAG,"disk"); return; }
    sony_disk_backend be; sd_disk_backend(&ls_disk, &be);
    sony_attach_backend(&ls_mj, 0, &be);
    m68k_reset(&ls_ci, &ls_mi);
    m68k_reset(&ls_cj, &ls_mj);
    if (!m68k_dispatcher_init_ex(&ls_dj, &ls_cj, BOARD_JIT_ARENA_KB, CC_MODE_LRU)) {
        ESP_LOGE(TAG, "JIT init failed"); return;
    }
    ESP_LOGI(TAG, "reset PC=0x%06" PRIX32 "; stepping...", ls_cj.pc);

    const u64 cap = 400ull * 1000 * 1000;
    u64 step = 0;
    while (ls_cj.cycles < cap && !ls_cj.halted && !ls_ci.halted) {
        u32 pcb = ls_cj.pc;
        u64 cycb = ls_cj.cycles;
        m68k_cpu pre = ls_cj;
        /* Near the hang, trace the block PC BEFORE running it (no flush —
         * the USB-JTAG ring still drains while the main task spins, so the
         * last PC emitted before the board goes silent is the block whose
         * native code failed to return). */
        if (cycb >= BOARD_LS_RAMCMP_FROM)
            printf("B %06" PRIX32 " a1=%08" PRIX32 " d5=%08" PRIX32 "\n",
                   pcb, ls_cj.a[1], ls_cj.d[5]);
        sony_set_vm(&ls_mj);
        m68k_dispatcher_run_until(&ls_dj, ls_cj.cycles + 1);   /* ~one JIT block */
        sony_set_vm(&ls_mi);
        m68k_run_until(&ls_ci, ls_cj.cycles);                  /* interp to same cycle */

        int bad = 0;
        for (int r = 0; r < 8; r++) {
            if (ls_ci.d[r] != ls_cj.d[r]) bad = 1;
            if (ls_ci.a[r] != ls_cj.a[r]) bad = 1;
        }
        if (ls_ci.pc != ls_cj.pc) bad = 1;
        if (ls_ci.sr != ls_cj.sr) bad = 1;
        if (ls_ci.cycles != ls_cj.cycles) bad = 1;
        /* Also catch a diverged store before it propagates into registers.
         * The full-RAM memcmp is expensive, so only run it once we're near
         * the known register-divergence cycle (override with -DBOARD_LS_RAMCMP_FROM). */
        if (!bad && cycb >= BOARD_LS_RAMCMP_FROM &&
            memcmp(ls_mi.ram, ls_mj.ram, ls_mi.ram_size) != 0) bad = 1;

        /* A divergence whose block touched a peripheral MMIO address is a
         * timing artifact, not a codegen bug: the JIT ticks the VIA/IWM/SCC
         * once per block while the interpreter ticks per instruction, so an
         * MMIO register read returns different bits. Re-sync the JIT engine
         * to the interpreter (registers + peripheral state) and continue, so
         * we can reach a *real* (RAM/compute) divergence. */
        static u64 mmio_skips = 0;
        if (bad) {
            bool mmio = false;
            for (int r = 0; r < 8; r++)
                if (pre.a[r] >= 0x500000u && pre.a[r] < 0x1000000u) mmio = true;
            if (mmio && memcmp(ls_mi.ram, ls_mj.ram, ls_mi.ram_size) == 0) {
                /* resync cj <- ci (keep cj's mem pointer) and peripherals */
                struct mac_mem *mjp = ls_cj.mem;
                ls_cj = ls_ci;  ls_cj.mem = mjp;  ls_cj.current_block = NULL;
                ls_mj.via = ls_mi.via; ls_mj.rtc = ls_mi.rtc;
                ls_mj.scc = ls_mi.scc;   /* IWM unused (Sony driver) */
                if ((++mmio_skips & 0x3FFu) == 0)
                    ESP_LOGI(TAG, "mmio-timing resync #%llu at cyc=%llu pc=0x%06" PRIX32,
                             (unsigned long long)mmio_skips,
                             (unsigned long long)ls_cj.cycles, ls_cj.pc);
                step++;
                continue;
            }
        }
        if (bad) {
            printf("\n[LOCKSTEP] *** DIVERGENCE *** step=%llu block PC=0x%06" PRIX32
                   " cyc %llu..%llu\n", (unsigned long long)step, pcb,
                   (unsigned long long)cycb, (unsigned long long)ls_cj.cycles);
            for (int r = 0; r < 8; r++)
                if (ls_ci.d[r] != ls_cj.d[r])
                    printf("  D%d interp=%08" PRIX32 " jit=%08" PRIX32 "\n",
                           r, ls_ci.d[r], ls_cj.d[r]);
            for (int r = 0; r < 8; r++)
                if (ls_ci.a[r] != ls_cj.a[r])
                    printf("  A%d interp=%08" PRIX32 " jit=%08" PRIX32 "\n",
                           r, ls_ci.a[r], ls_cj.a[r]);
            if (ls_ci.pc != ls_cj.pc)
                printf("  PC interp=%06" PRIX32 " jit=%06" PRIX32 "\n", ls_ci.pc, ls_cj.pc);
            if (ls_ci.sr != ls_cj.sr)
                printf("  SR interp=%04X jit=%04X\n", ls_ci.sr, ls_cj.sr);
            if (ls_ci.cycles != ls_cj.cycles)
                printf("  cyc interp=%llu jit=%llu\n",
                       (unsigned long long)ls_ci.cycles, (unsigned long long)ls_cj.cycles);
            printf("  pre-block: D0=%08" PRIX32 " D1=%08" PRIX32 " A0=%08" PRIX32
                   " A1=%08" PRIX32 " A7=%08" PRIX32 " SR=%04X\n",
                   pre.d[0], pre.d[1], pre.a[0], pre.a[1], pre.a[7], pre.sr);
            {   /* first few RAM bytes that differ (a mis-stored value) */
                int diffs = 0;
                for (u32 a = 0; a < ls_mi.ram_size && diffs < 8; a++)
                    if (ls_mi.ram[a] != ls_mj.ram[a]) {
                        printf("  RAM[%06" PRIX32 "] interp=%02X jit=%02X\n",
                               a, ls_mi.ram[a], ls_mj.ram[a]);
                        diffs++;
                    }
                if (diffs == 0) printf("  RAM: identical (register-only divergence)\n");
            }
            printf("  block instructions:\n");
            u32 ppc = pcb;
            for (int i = 0; i < 24; i++) {
                m68k_decoded dec = m68k_decode_at(&ls_ci, ppc);
                printf("    %06" PRIX32 ": %04X%s\n", ppc, dec.opcode,
                       dec.ends_block ? "  (ends block)" : "");
                if (dec.ends_block) break;
                ppc += dec.length;
            }
            fflush(stdout);
            return;
        }
        if ((++step & 0x3FFFFu) == 0)
            ESP_LOGI(TAG, "lockstep ok: %llu blocks, cyc=%llu pc=0x%06" PRIX32,
                     (unsigned long long)step, (unsigned long long)ls_cj.cycles, ls_cj.pc);
    }
    ESP_LOGW(TAG, "lockstep: no divergence through cyc=%llu (%llu blocks)",
             (unsigned long long)ls_cj.cycles, (unsigned long long)step);
}
#endif /* BOARD_LOCKSTEP */

void app_main(void) {
#if BOARD_QEMU_SELFTEST
    selftest_main();
    while (1) vTaskDelay(portMAX_DELAY);
#elif BOARD_LOCKSTEP
    lockstep_main();
    while (1) vTaskDelay(portMAX_DELAY);
#else
    uart_ctl_init();   /* USB-Serial-JTAG remote control (screen dump + input) */
    ESP_LOGI(TAG, "mac68k-jit-xtensa on M5Stack PaperS3 (headless: CPU + HDD)");
    ESP_LOGI(TAG, "free internal=%u KB  PSRAM=%u KB",
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));

    if (sd_mount() != 0) {
        ESP_LOGE(TAG, "no SD card — cannot boot. Halting.");
        goto park;
    }

    /* Guest RAM (from PSRAM via calloc). */
    mac_mem_init(&s_mem, BOARD_RAM_MB * 1024u * 1024u);
    s_mem.serial_sink = serial_sink;
    if (!s_mem.ram) {
        ESP_LOGE(TAG, "guest RAM alloc (%u MB) failed", BOARD_RAM_MB);
        goto park;
    }
    ESP_LOGI(TAG, "guest RAM = %u MB", BOARD_RAM_MB);

    /* ROM from internal flash. mac_load_rom() copies it into RAM and patches
     * the .Sony driver there so disk I/O is served as logical sectors. */
    u32 rlen = (u32)(macplus_rom_end - macplus_rom_start);
    if (!mac_load_rom(&s_mem, macplus_rom_start, rlen)) {
        ESP_LOGE(TAG, "mac_load_rom failed (%u bytes)", rlen);
        goto park;
    }
    ESP_LOGI(TAG, "ROM loaded from flash: %u bytes, .Sony patched", rlen);

    /* Drive 1: the boot volume, attached at reset (SD-streamed). */
    if (!attach_disk(&s_boot, 0, BOARD_PATH_BOOT, false)) {
        ESP_LOGE(TAG, "boot disk attach failed — halting");
        goto park;
    }

    /* Reset the CPU and bring up the chosen engine. */
    m68k_reset(&s_cpu, &s_mem);
    ESP_LOGI(TAG, "reset: PC=0x%06" PRIX32 " SSP=0x%06" PRIX32 "  engine=%s",
             s_cpu.pc, s_cpu.a[7], BOARD_USE_JIT ? "JIT" : "interp");

#if BOARD_USE_JIT
    if (!m68k_dispatcher_init_ex(&s_disp, &s_cpu,
                                 BOARD_JIT_ARENA_KB, BOARD_JIT_EVICT)) {
        ESP_LOGE(TAG, "JIT init failed (arena=%u KB) — is IRAM exhausted?",
                 BOARD_JIT_ARENA_KB);
        goto park;
    }
    m68k_dispatcher_set_compile_threshold(&s_disp, BOARD_JIT_HOT_THRESHOLD);
    ESP_LOGI(TAG, "JIT arena=%u KB evict=%d hot-threshold=%u",
             (unsigned)(s_disp.arena_cap / 1024), BOARD_JIT_EVICT,
             (unsigned)BOARD_JIT_HOT_THRESHOLD);
#endif

    /* Run loop. We advance in cycle chunks so we can (a) hot-insert drive 2
     * after boot and (b) emit a periodic heartbeat. mac_mem_tick / VBL /
     * sony_service all run inside the engine's run_until. */
    int64_t t0 = esp_timer_get_time();
    int64_t last_log = t0;
    u64 last_cycles = 0;

    while (!s_cpu.halted) {
        /* Small chunks while a timed click/key sequence is in flight so the
         * press/release land spaced in guest time; large chunks otherwise. */
        u64 chunk = uart_ctl_busy() ? 100000ull : 2000000ull;
        u64 next = s_cpu.cycles + chunk;
#if BOARD_USE_JIT
        m68k_dispatcher_run_until(&s_disp, next);
#else
        m68k_run_until(&s_cpu, next);
#endif
        uart_ctl_poll(&s_cpu, &s_mem);   /* host commands + due input events */
        /* Hot-insert the Infinite HD into drive 2 once the desktop is up. */
        if (!s_hd_open && s_cpu.cycles >= BOARD_HD_INSERT_CYCLE) {
            s_hd_open = true;   /* attempt once, regardless of outcome */
            if (attach_disk(&s_hd, 1, BOARD_PATH_HD, false))
                ESP_LOGI(TAG, "drive 2 inserted (Infinite HD)");
            else
                ESP_LOGW(TAG, "drive 2 (%s) not attached — continuing",
                         BOARD_PATH_HD);
        }

        int64_t now = esp_timer_get_time();
        if (now - last_log >= 3000000) {        /* heartbeat every 3 s */
            int64_t win = now - last_log;       /* window wall-time (us) */
            double mhz = (double)(s_cpu.cycles - last_cycles) / (double)win;
            /* SD-read activity this window. */
            static uint64_t p_calls, p_txn, p_bytes; static int64_t p_us;
            uint64_t c_calls, c_txn, c_bytes; int64_t c_us;
            sd_disk_stats(&c_calls, &c_txn, &c_bytes, &c_us);
            /* m68k_step invocations (interp steps / JIT helper fallbacks). */
            static u64 p_instrs;
            u64 d_instrs = s_cpu.instrs - p_instrs;
#if BOARD_USE_JIT
            static u64 p_bc, p_bx, p_rst, p_smc, p_fb;
            ESP_LOGI(TAG, "t=%llds cyc=%llu pc=0x%06" PRIX32 " %.3f MHz | "
                     "blk %llu/%llu rst %llu smc %llu fb %llu steps %llu | "
                     "sd %llurd %llutxn %lluKB %lldms(%.0f%%)",
                     (long long)((now - t0) / 1000000),
                     (unsigned long long)s_cpu.cycles, s_cpu.pc, mhz,
                     (unsigned long long)(s_disp.blocks_compiled - p_bc),
                     (unsigned long long)(s_disp.blocks_executed - p_bx),
                     (unsigned long long)(s_disp.arena_resets - p_rst),
                     (unsigned long long)(s_disp.smc_invalidations - p_smc),
                     (unsigned long long)(s_disp.interp_fallbacks - p_fb),
                     (unsigned long long)d_instrs,
                     (unsigned long long)(c_calls - p_calls),
                     (unsigned long long)(c_txn - p_txn),
                     (unsigned long long)((c_bytes - p_bytes) / 1024),
                     (long long)((c_us - p_us) / 1000),
                     100.0 * (double)(c_us - p_us) / (double)win);
            p_bc = s_disp.blocks_compiled; p_bx = s_disp.blocks_executed;
            p_rst = s_disp.arena_resets;   p_smc = s_disp.smc_invalidations;
            p_fb = s_disp.interp_fallbacks;
            ESP_LOGI(TAG, "   smc-sample: write=0x%06" PRIX32
                     " dropped blk[0x%06" PRIX32 "..0x%06" PRIX32 "] (in=%d)",
                     s_disp.dbg_smc_w, s_disp.dbg_smc_pcs, s_disp.dbg_smc_pce,
                     (s_disp.dbg_smc_w >= s_disp.dbg_smc_pcs &&
                      s_disp.dbg_smc_w <  s_disp.dbg_smc_pce));
#else
            ESP_LOGI(TAG, "t=%llds cyc=%llu pc=0x%06" PRIX32 " %.3f MHz | "
                     "steps %llu | sd %llurd %llutxn %lluKB %lldms(%.0f%%)",
                     (long long)((now - t0) / 1000000),
                     (unsigned long long)s_cpu.cycles, s_cpu.pc, mhz,
                     (unsigned long long)d_instrs,
                     (unsigned long long)(c_calls - p_calls),
                     (unsigned long long)(c_txn - p_txn),
                     (unsigned long long)((c_bytes - p_bytes) / 1024),
                     (long long)((c_us - p_us) / 1000),
                     100.0 * (double)(c_us - p_us) / (double)win);
#endif
            p_calls = c_calls; p_txn = c_txn; p_bytes = c_bytes; p_us = c_us;
            p_instrs = s_cpu.instrs;
            last_log = now;
            last_cycles = s_cpu.cycles;
        }
    }

    ESP_LOGW(TAG, "CPU halted at PC=0x%06" PRIX32 " (exit=%d)",
             s_cpu.pc, s_cpu.exit_code);

park:
    while (1) vTaskDelay(portMAX_DELAY);
#endif /* BOARD_QEMU_SELFTEST */
}
