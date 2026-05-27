/* se30_trace.c — boot tracer for the SE/30 ROM.
 *
 * Stand-alone driver: loads roms/MacIIx.ROM, runs the interpreter under
 * MAC_MODEL_SE30 for a configurable cycle budget, and dumps:
 *   - the first visit to each 256-byte PC region (with cycle stamp)
 *   - a PC histogram of "hot" regions
 *   - the exception log
 *
 * Usage: build/se30_trace [max_cycles=200000000]
 *
 * Built by tools/build_se30_trace.sh.
 */

#include "m68k_cpu.h"
#include "m68k_interp.h"
#include "mac_mem.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static u32 read_file(const char *path, u8 **out) {
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); exit(2); }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    u8 *buf = (u8 *)malloc((size_t)n);
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) { perror("read"); exit(2); }
    fclose(f);
    *out = buf;
    return (u32)n;
}

/* PC visit map — one bit per 16-byte ROM PC region (32 KB total for 256
 * KB ROM, covering 0x40800000-0x4083FFFF). */
#define RGN_SIZE   16u
#define RGN_COUNT  (0x40000u / RGN_SIZE)
static u8  visited[RGN_COUNT];           /* 0/1 */
static u64 first_cyc[RGN_COUNT];

/* Per-PC visit counter for the most-frequented top-PCs report. */
#define HOT_BUCKETS 16384u
static struct { u32 pc; u64 n; } hot[HOT_BUCKETS];
static u32 hot_n;

static void note_pc(u32 pc, u64 cyc) {
    if (pc >= 0x40800000u && pc < 0x40840000u) {
        u32 idx = (pc - 0x40800000u) / RGN_SIZE;
        if (!visited[idx]) {
            visited[idx] = 1;
            first_cyc[idx] = cyc;
        }
    }
    /* Hot-PC counter (linear probe). */
    u32 h = (pc * 2654435761u) % HOT_BUCKETS;
    for (u32 i = 0; i < 32; i++) {
        u32 j = (h + i) % HOT_BUCKETS;
        if (hot[j].n == 0) { hot[j].pc = pc; hot[j].n = 1; hot_n++; return; }
        if (hot[j].pc == pc) { hot[j].n++; return; }
    }
}

int main(int argc, char **argv) {
    u64 max_cycles = (argc > 1) ? strtoull(argv[1], NULL, 0) : 200000000ull;

    mac_mem mem;
    mac_mem_init_ex(&mem, MAC_MODEL_SE30, 8u * 1024u * 1024u);

    u8 *rom_data = NULL;
    u32 rom_len = read_file("roms/MacIIx.ROM", &rom_data);
    if (!mac_load_rom(&mem, rom_data, rom_len)) {
        fprintf(stderr, "mac_load_rom failed\n"); return 3;
    }
    free(rom_data);
    fprintf(stderr, "[se30_trace] loaded MacIIx.ROM (%u bytes)\n", rom_len);

    m68k_cpu cpu;
    m68k_reset(&cpu, &mem);
    fprintf(stderr, "[se30_trace] reset PC=0x%08X SSP=0x%08X SR=0x%04X\n",
            cpu.pc, cpu.a[7], cpu.sr);

    u64 sample_every = 32;
    u64 next_sample = 0;
    /* Single-step so we can hook every instruction's PC. m68k_step doesn't
     * have a callback, so we just run one instruction at a time and read
     * cpu.pc between steps. That's slow but fine for a debug trace. */
    while (cpu.cycles < max_cycles && !cpu.halted) {
        if (cpu.cycles >= next_sample) {
            note_pc(cpu.pc, cpu.cycles);
            next_sample = cpu.cycles + sample_every;
        }
        u64 chunk_end = cpu.cycles + 1024;
        if (chunk_end > max_cycles) chunk_end = max_cycles;
        m68k_run_until(&cpu, chunk_end);
    }
    note_pc(cpu.pc, cpu.cycles);

    fprintf(stderr, "[se30_trace] halt=%d pc=0x%08X cycles=%llu instrs=%llu\n",
            cpu.halted, cpu.pc,
            (unsigned long long)cpu.cycles,
            (unsigned long long)cpu.instrs);

    /* First-visit timeline: print each newly-visited ROM region. */
    fprintf(stderr, "\n[se30_trace] first-visit timeline (ROM regions):\n");
    u64 prev_cyc = 0;
    for (u32 i = 0; i < RGN_COUNT; i++) {
        if (!visited[i]) continue;
        u32 pc = 0x40800000u + i * RGN_SIZE;
        fprintf(stderr, "  PC=0x%08X cyc=%llu (+%llu)\n",
                pc, (unsigned long long)first_cyc[i],
                (unsigned long long)(first_cyc[i] - prev_cyc));
        prev_cyc = first_cyc[i];
    }

    /* Top hot PCs (cycle stuck spots). */
    fprintf(stderr, "\n[se30_trace] hot PCs (top 20 by sample count):\n");
    for (int t = 0; t < 20; t++) {
        u32 best = 0;
        for (u32 i = 0; i < HOT_BUCKETS; i++) {
            if (hot[i].n > hot[best].n) best = i;
        }
        if (hot[best].n == 0) break;
        fprintf(stderr, "  PC=0x%08X  samples=%llu\n",
                hot[best].pc, (unsigned long long)hot[best].n);
        hot[best].n = 0;
    }

    /* Last few exceptions. */
    fprintf(stderr, "\n[se30_trace] last %u exceptions:\n",
            m68k_exc_n < 64u ? m68k_exc_n : 64u);
    u32 start = m68k_exc_n >= 16u ? m68k_exc_n - 16u : 0;
    for (u32 i = start; i < m68k_exc_n; i++) {
        u32 *e = m68k_exc_log[i & 63];
        fprintf(stderr, "  #%u vec=%u pc=0x%08X cyc=%u\n",
                i, e[0], e[1], e[2]);
    }

    mac_mem_free(&mem);
    return 0;
}
