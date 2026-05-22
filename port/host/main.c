/* Host CLI driver for the mac68k-jit-xtensa emulator.
 *
 * Runs a 68000 program either under the reference interpreter or the JIT
 * (the latter executing through the in-tree Xtensa simulator, since the
 * host is not an Xtensa). With no ROM path it runs the built-in demo. */

#include "m68k_cpu.h"
#include "m68k_interp.h"
#include "mac_mem.h"
#include "demo_rom.h"
#include "dispatcher.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Write the 512x342 1bpp Mac framebuffer as an 8-bit grayscale BMP so the
 * boot screen can be inspected. Mac convention: a set bit is a black pixel. */
static void write_screen_bmp(const mac_mem *m, const char *path) {
    int W = MAC_SCREEN_W, H = MAC_SCREEN_H;
    int row = (W + 3) & ~3, isz = row * H;
    FILE *f = fopen(path, "wb");
    if (!f) return;
    unsigned char hdr[54] = {0};
    hdr[0] = 'B'; hdr[1] = 'M';
    *(int *)(hdr + 2) = 54 + 1024 + isz;
    *(int *)(hdr + 10) = 54 + 1024;
    *(int *)(hdr + 14) = 40;
    *(int *)(hdr + 18) = W; *(int *)(hdr + 22) = H;
    hdr[26] = 1; hdr[28] = 8;
    *(int *)(hdr + 34) = isz;
    fwrite(hdr, 1, 54, f);
    for (int i = 0; i < 256; i++) {
        unsigned char pe[4] = { (unsigned char)i, (unsigned char)i,
                                (unsigned char)i, 0 };
        fwrite(pe, 1, 4, f);
    }
    unsigned char *ln = (unsigned char *)calloc(1, (size_t)row);
    for (int y = H - 1; y >= 0; y--) {
        for (int x = 0; x < W; x++) {
            u32 a = m->fb_base + (u32)y * (W / 8) + (u32)(x >> 3);
            u8 byte = m->ram[a % m->ram_size];
            ln[x] = ((byte >> (7 - (x & 7))) & 1) ? 0 : 255;
        }
        fwrite(ln, 1, (size_t)row, f);
    }
    free(ln);
    fclose(f);
}

static void serial_cb(void *ctx, u8 b) {
    (void)ctx;
    fputc((int)b, stdout);
    fflush(stdout);
}

static int read_file(const char *path, u8 **out, u32 *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 4 * 1024 * 1024) { fclose(f); return -2; }
    u8 *buf = (u8 *)malloc((size_t)sz);
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) { free(buf); fclose(f); return -3; }
    fclose(f);
    *out = buf;
    *len = (u32)sz;
    return 0;
}

static void usage(const char *a0) {
    fprintf(stderr,
        "usage: %s [--interp|--jit] [--rom] [--max-cycles N] [--ram-mb M] [file]\n"
        "  --interp        run with the reference interpreter (default)\n"
        "  --jit           run with the JIT (via the host Xtensa simulator)\n"
        "  --rom           load `file` as a Macintosh ROM (mapped at 0x400000,\n"
        "                  overlaid at 0x0 for boot) instead of a raw RAM image\n"
        "  --disk D        insert raw 800K floppy image D into the IWM drive\n"
        "  --screenshot P  write the 512x342 framebuffer to BMP file P at exit\n"
        "  --max-cycles N  stop after N cycles (default 200M)\n"
        "  --ram-mb M      RAM size in MiB (default 1)\n"
        "  file            raw 68k image at 0x0, or a Mac ROM with --rom;\n"
        "                  omit for the built-in demo\n",
        a0);
}

int main(int argc, char **argv) {
    bool use_jit = false;
    bool is_rom = false;
    u64 max_cycles = 200000000ull;
    u32 ram_mb = 1;
    const char *rom_path = NULL;
    const char *disk_path = NULL;
    const char *shot_path = NULL;

    for (int i = 1; i < argc; i++) {
        if      (!strcmp(argv[i], "--interp")) use_jit = false;
        else if (!strcmp(argv[i], "--jit"))    use_jit = true;
        else if (!strcmp(argv[i], "--rom"))    is_rom = true;
        else if (!strcmp(argv[i], "--disk") && i + 1 < argc)
            disk_path = argv[++i];
        else if (!strcmp(argv[i], "--screenshot") && i + 1 < argc)
            shot_path = argv[++i];
        else if (!strcmp(argv[i], "--max-cycles") && i + 1 < argc)
            max_cycles = strtoull(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--ram-mb") && i + 1 < argc)
            ram_mb = (u32)strtoul(argv[++i], NULL, 0);
        else if (argv[i][0] == '-') { usage(argv[0]); return 1; }
        else rom_path = argv[i];
    }
    if (ram_mb == 0) ram_mb = 1;

    mac_mem mem;
    mac_mem_init(&mem, ram_mb * 1024u * 1024u);
    mem.serial_sink = serial_cb;

    if (rom_path) {
        u8 *rom = NULL; u32 rlen = 0;
        if (read_file(rom_path, &rom, &rlen) != 0) {
            fprintf(stderr, "failed to read %s\n", rom_path);
            return 2;
        }
        if (is_rom) {
            if (!mac_load_rom(&mem, rom, rlen)) {
                fprintf(stderr, "rom too large / invalid: %s\n", rom_path);
                free(rom);
                return 3;
            }
            fprintf(stderr, "[host] loaded Mac ROM %s (%u bytes) at 0x%06X, "
                            "overlaid at 0x0\n", rom_path, rlen, MAC_ROM_BASE);
        } else {
            mac_load_ram_image(&mem, 0, rom, rlen);
            fprintf(stderr, "[host] loaded %s (%u bytes) at 0x0\n",
                    rom_path, rlen);
        }
        free(rom);
        if (disk_path) {
            u8 *disk = NULL; u32 dlen = 0;
            if (read_file(disk_path, &disk, &dlen) != 0) {
                fprintf(stderr, "failed to read disk %s\n", disk_path);
                return 2;
            }
            if (mac_insert_disk(&mem, disk, dlen, false))
                fprintf(stderr, "[host] inserted floppy %s (%u bytes)\n",
                        disk_path, dlen);
            free(disk);
        }
    } else {
        u8 img[DEMO_ROM_MAX];
        u32 len = demo_rom_build(img, mem.fb_base);
        mac_load_ram_image(&mem, 0, img, len);
        fprintf(stderr, "[host] built-in demo (%u bytes), fb=0x%06X\n",
                len, mem.fb_base);
    }

    m68k_cpu cpu;
    m68k_reset(&cpu, &mem);
    fprintf(stderr, "[host] reset: PC=0x%06X SSP=0x%06X  mode=%s\n",
            cpu.pc, cpu.a[7], use_jit ? "JIT" : "interp");

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    m68k_dispatcher disp;
    if (use_jit) {
        if (!m68k_dispatcher_init(&disp, &cpu)) {
            fprintf(stderr, "JIT init failed\n");
            return 4;
        }
        m68k_dispatcher_run_until(&disp, max_cycles);
    } else {
        m68k_run_until(&cpu, max_cycles);
    }

    clock_gettime(CLOCK_MONOTONIC, &t1);
    double us = (t1.tv_sec - t0.tv_sec) * 1e6 + (t1.tv_nsec - t0.tv_nsec) / 1e3;
    double mhz = us > 0 ? (double)cpu.cycles / us : 0.0;

    fprintf(stderr,
        "\n[host] %s halted=%d exit_code=%d PC=0x%06X cycles=%llu "
        "elapsed=%.0fus throughput=%.2fMHz\n",
        use_jit ? "JIT" : "interp", cpu.halted, cpu.exit_code, cpu.pc,
        (unsigned long long)cpu.cycles, us, mhz);
    if (use_jit) {
        fprintf(stderr,
            "[host] blocks=%llu/%llu inline_ops=%llu helper_ops=%llu "
            "chain=%llu/%llu resets=%llu\n",
            (unsigned long long)disp.blocks_compiled,
            (unsigned long long)disp.blocks_executed,
            (unsigned long long)disp.inline_ops_total,
            (unsigned long long)disp.helper_ops_total,
            (unsigned long long)disp.chain_hits,
            (unsigned long long)disp.chain_misses,
            (unsigned long long)disp.arena_resets);
        m68k_dispatcher_shutdown(&disp);
    }

    if (shot_path) {
        write_screen_bmp(&mem, shot_path);
        fprintf(stderr, "[host] screenshot written to %s\n", shot_path);
    }

    /* exit_code mirrors the value the guest wrote to the debug exit port. */
    return cpu.exit_code;
}
