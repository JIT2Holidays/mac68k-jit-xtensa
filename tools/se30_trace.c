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

/* MMIO trace — bucketed by addr. Records read/write counts and last value. */
#define MMIO_BUCKETS 16384u
static struct {
    u32 addr;
    u64 reads, writes;
    u32 last_rd_val, last_wr_val;
    u64 last_rd_cyc, last_wr_cyc;
} mmio[MMIO_BUCKETS];

static void note_mmio(u32 addr, u32 val, int is_write, int size, u64 cyc) {
    (void)size;
    u32 h = (addr * 2654435761u) % MMIO_BUCKETS;
    for (u32 i = 0; i < 64; i++) {
        u32 j = (h + i) % MMIO_BUCKETS;
        if (mmio[j].reads == 0 && mmio[j].writes == 0) {
            mmio[j].addr = addr;
        }
        if (mmio[j].addr == addr || (mmio[j].reads == 0 && mmio[j].writes == 0)) {
            mmio[j].addr = addr;
            if (is_write) {
                mmio[j].writes++;
                mmio[j].last_wr_val = val;
                mmio[j].last_wr_cyc = cyc;
            } else {
                mmio[j].reads++;
                mmio[j].last_rd_val = val;
                mmio[j].last_rd_cyc = cyc;
            }
            return;
        }
    }
}

/* Global pointer to active mac_mem so the mmio_log callback can read cycles. */
static struct m68k_cpu *g_cpu;
static void mmio_log_cb(mac_mem *m, u32 addr, u32 val, int is_write, int size) {
    (void)m;
    note_mmio(addr, val, is_write, size, g_cpu ? g_cpu->cycles : 0);
}

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
    /* Default 128 MB so the ROM's RAM-probe loop terminates cleanly.
     * SE30_RAM_MB env overrides — useful for generating smaller
     * snapshots for ctest. */
    u32 ram_mb = 128;
    const char *rm = getenv("SE30_RAM_MB");
    if (rm) { unsigned long v = strtoul(rm, NULL, 0); if (v) ram_mb = (u32)v; }
    mac_mem_init_ex(&mem, MAC_MODEL_SE30, ram_mb * 1024u * 1024u);

    u8 *rom_data = NULL;
    u32 rom_len = read_file("roms/MacIIx.ROM", &rom_data);
    if (!mac_load_rom(&mem, rom_data, rom_len)) {
        fprintf(stderr, "mac_load_rom failed\n"); return 3;
    }
    free(rom_data);
    fprintf(stderr, "[se30_trace] loaded MacIIx.ROM (%u bytes)\n", rom_len);

    /* M7.6v — ROM patch at 0x40802C6C is now DEFAULT-ON for the trace
     * tool. The unconditional BRA at that offset routes execution to a
     * "diagnostic" path at 0x40802E96 that begins with MOVEQ #0,D7 —
     * thus clearing D7 bit 16, which the post-ASC loop later tests and
     * branches on. The normal-boot path at 0x40802C70 does BSET #16,D7
     * + BSET #22,D7 + enables cache, but it is unreachable from this
     * entry without the patch.
     *
     * Why this is a workaround, not the real fix: the real ROM presumably
     * relies on diagnostic-path hardware tests succeeding and then BSET'ing
     * bit 16 from within the diagnostic path. Our hardware model doesn't
     * fully satisfy those tests (ASC/VIA2/ADB are partial stubs), so the
     * ROM stays in diagnostic mode forever. Patching past the BRA lets
     * the next stall surface so we can keep iterating.
     *
     * Set SE30_NO_PATCH=1 to disable the patch and reproduce the original
     * stall at PC=0x40802EE0 (described in the M7.6t roadmap memory).
     */
    if (mem.rom && rom_len > 0x2C70 && !getenv("SE30_NO_PATCH")) {
        mem.rom[0x2C6C] = 0x4E; mem.rom[0x2C6D] = 0x71;  /* NOP */
        mem.rom[0x2C6E] = 0x4E; mem.rom[0x2C6F] = 0x71;  /* NOP */
        fprintf(stderr, "[se30_trace] patched 0x40802C6C BRA → NOP NOP\n");
    }

    /* SE30_PATCH_32AC=1 — NOP the BEQ.S at 0x408032AC that takes the
     * RX_AVAIL=0 path back into the hot loop. Forces the ROM into the
     * RX-data-processing branch at 0x408032AE regardless of RX state.
     * Aggressive — useful for exploring what happens past the
     * post-PMMU stall when serial input isn't available. */
    if (mem.rom && rom_len > 0x32AD && getenv("SE30_PATCH_32AC")) {
        mem.rom[0x32AC] = 0x4E; mem.rom[0x32AD] = 0x71;  /* NOP */
        fprintf(stderr, "[se30_trace] patched 0x408032AC BEQ → NOP\n");
    }

    m68k_cpu cpu;
    m68k_reset(&cpu, &mem);
    g_cpu = &cpu;
    mac_mmio_log = mmio_log_cb;
    fprintf(stderr, "[se30_trace] reset PC=0x%08X SSP=0x%08X SR=0x%04X\n",
            cpu.pc, cpu.a[7], cpu.sr);

    /* SE30_SAVE_RESET=<path>: write a machine snapshot at reset (cycle 0)
     * so the diff-jit-trace harness can lockstep JIT vs interp from
     * the very first instruction. Matches write_snapshot()'s on-disk
     * layout in port/host/main.c — must stay in sync. */
    const char *save_reset = getenv("SE30_SAVE_RESET");
    if (save_reset) {
        FILE *sf = fopen(save_reset, "wb");
        if (sf) {
            u32 hdr[24] = {0};
            hdr[0] = 0x4D414331u;
            for (int i = 0; i < 8; i++) { hdr[1+i] = cpu.d[i]; hdr[9+i] = cpu.a[i]; }
            hdr[17] = cpu.pc; hdr[18] = cpu.sr;
            hdr[19] = cpu.usp; hdr[20] = cpu.ssp;
            hdr[21] = (u32)mem.model;
            fwrite(hdr, 4, 24, sf);
            fwrite(&mem.ram_size, 4, 1, sf);
            fwrite(mem.ram, 1, mem.ram_size, sf);
            fwrite(&mem.rom_size, 4, 1, sf);
            fwrite(mem.rom, 1, mem.rom_size, sf);
            fclose(sf);
            fprintf(stderr, "[se30_trace] saved reset snapshot to %s\n", save_reset);
        }
    }

    /* Optional: inject a byte into SCC channel A at a fixed cycle to
     * test whether the post-ASC outer loop is purely SCC-poll-waiting.
     * SE30_INJECT_BYTE=<hex> SE30_INJECT_AT=<cyc> env vars. */
    u8 inject_byte = 0;
    u64 inject_at = ~0ull;
    const char *ib = getenv("SE30_INJECT_BYTE");
    const char *ia = getenv("SE30_INJECT_AT");
    if (ib && ia) {
        inject_byte = (u8)strtoul(ib, NULL, 0);
        inject_at = strtoull(ia, NULL, 0);
        fprintf(stderr, "[se30_trace] will inject SCC byte 0x%02X at cyc=%llu\n",
                inject_byte, (unsigned long long)inject_at);
    }
    bool injected = (inject_at == ~0ull);

    /* SE30_INJECT_STREAM=<hex bytes>: after inject_at fires, keep refilling
     * SCC ch A RX whenever it's empty, cycling through the byte stream.
     * Lets us test how the ROM consumes serial input — e.g.
     * SE30_INJECT_STREAM=0D0A1B5B5D injects "\r\n\e[]" repeatedly. */
    u8 stream[64]; u32 stream_n = 0, stream_idx = 0;
    const char *is = getenv("SE30_INJECT_STREAM");
    if (is) {
        for (u32 i = 0; is[i] && is[i+1] && stream_n < 64; i += 2) {
            char hex[3] = { is[i], is[i+1], 0 };
            stream[stream_n++] = (u8)strtoul(hex, NULL, 16);
        }
        fprintf(stderr, "[se30_trace] inject stream: %u bytes, cycle every poll\n",
                stream_n);
    }

    u64 sample_every = 32;
    u64 next_sample = 0;
    /* SE30_DUMP_AT=<cyc>: after reaching that cycle, single-step the next
     * SE30_DUMP_N instructions (default 500) and dump each PC + dn/an
     * registers to stderr. Enough to see one full hot-loop iteration. */
    u64 dump_at = ~0ull;
    u32 dump_n = 500;
    const char *da = getenv("SE30_DUMP_AT");
    if (da) {
        dump_at = strtoull(da, NULL, 0);
        const char *dn = getenv("SE30_DUMP_N");
        if (dn) dump_n = (u32)strtoul(dn, NULL, 0);
        fprintf(stderr, "[se30_trace] will dump %u insns at cyc=%llu\n",
                dump_n, (unsigned long long)dump_at);
    }
    /* Single-step so we can hook every instruction's PC. m68k_step doesn't
     * have a callback, so we just run one instruction at a time and read
     * cpu.pc between steps. That's slow but fine for a debug trace. */
    while (cpu.cycles < max_cycles && !cpu.halted) {
        if (cpu.cycles >= next_sample) {
            note_pc(cpu.pc, cpu.cycles);
            next_sample = cpu.cycles + sample_every;
        }
        if (!injected && cpu.cycles >= inject_at) {
            mac_scc_rx(&mem, 1, inject_byte);
            fprintf(stderr, "[se30_trace] injected SCC byte 0x%02X at cyc=%llu\n",
                    inject_byte, (unsigned long long)cpu.cycles);
            injected = true;
        }
        /* Stream-mode: keep ch A RX queue refilled. */
        if (stream_n > 0 && injected && !mem.scc.ch[1].rx_avail) {
            mac_scc_rx(&mem, 1, stream[stream_idx % stream_n]);
            stream_idx++;
        }
        if (dump_at != ~0ull && cpu.cycles >= dump_at && dump_n > 0) {
            for (u32 i = 0; i < dump_n && !cpu.halted; i++) {
                u32 pc = cpu.pc;
                u16 op = mac_read16(&mem, pc);
                fprintf(stderr,
                    "  STEP pc=%08X op=%04X "
                    "d0-d7=%08X,%08X,%08X,%08X,%08X,%08X,%08X,%08X "
                    "a0-a7=%08X,%08X,%08X,%08X,%08X,%08X,%08X,%08X sr=%04X\n",
                    pc, op,
                    cpu.d[0], cpu.d[1], cpu.d[2], cpu.d[3],
                    cpu.d[4], cpu.d[5], cpu.d[6], cpu.d[7],
                    cpu.a[0], cpu.a[1], cpu.a[2], cpu.a[3],
                    cpu.a[4], cpu.a[5], cpu.a[6], cpu.a[7], cpu.sr);
                m68k_step(&cpu);
            }
            dump_n = 0;
            continue;
        }
        u64 chunk_end = cpu.cycles + 1024;
        if (chunk_end > max_cycles) chunk_end = max_cycles;
        m68k_run_until(&cpu, chunk_end);
    }
    note_pc(cpu.pc, cpu.cycles);

    fprintf(stderr, "[se30_trace] halt=%d pc=0x%08X cycles=%llu instrs=%llu "
            "last_fault_addr=0x%08X bus_err_pending=0x%08X d7=0x%08X a6=0x%08X\n",
            cpu.halted, cpu.pc,
            (unsigned long long)cpu.cycles,
            (unsigned long long)cpu.instrs,
            cpu.fault_addr, cpu.bus_error_pending, cpu.d[7], cpu.a[6]);

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

    /* Total MMIO ops. */
    {
        u64 tot_r = 0, tot_w = 0; u32 nbuckets = 0;
        for (u32 i = 0; i < MMIO_BUCKETS; i++) {
            tot_r += mmio[i].reads; tot_w += mmio[i].writes;
            if (mmio[i].reads + mmio[i].writes) nbuckets++;
        }
        fprintf(stderr, "\n[se30_trace] MMIO totals: %u unique addrs, %llu reads, %llu writes\n",
                nbuckets, (unsigned long long)tot_r, (unsigned long long)tot_w);
    }

    /* MMIO access summary — top 60 most-accessed addresses. */
    fprintf(stderr, "\n[se30_trace] hot MMIO addresses (top 60 by access count):\n");
    for (int t = 0; t < 60; t++) {
        u32 best = 0;
        u64 best_n = 0;
        for (u32 i = 0; i < MMIO_BUCKETS; i++) {
            u64 n = mmio[i].reads + mmio[i].writes;
            if (n > best_n) { best_n = n; best = i; }
        }
        if (best_n == 0) break;
        fprintf(stderr, "  0x%08X  rd=%llu (last=0x%X @cyc=%llu)  wr=%llu (last=0x%X @cyc=%llu)\n",
                mmio[best].addr,
                (unsigned long long)mmio[best].reads,
                mmio[best].last_rd_val,
                (unsigned long long)mmio[best].last_rd_cyc,
                (unsigned long long)mmio[best].writes,
                mmio[best].last_wr_val,
                (unsigned long long)mmio[best].last_wr_cyc);
        mmio[best].reads = 0;
        mmio[best].writes = 0;
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
