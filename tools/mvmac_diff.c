/* mvmac_diff.c — instruction-lockstep differential between this project's
 * 68000 interpreter and mini vMac's CPU core.
 *
 * Loads a machine snapshot (written by `mac68k_host` in MAC68K_CPDEBUG
 * mode) into both CPUs and steps them in lockstep, comparing the data
 * and address registers and the PC after every instruction. The first
 * mismatch is the instruction this project's interpreter gets wrong.
 *
 *   ./mvmac_diff <snapshot> [max-steps]
 */

#include "m68k_cpu.h"
#include "m68k_interp.h"
#include "mac_mem.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --- device-read replay ----------------------------------------------
 * This project's interpreter is stepped first; its memory-mapped device
 * reads are queued here and replayed to mini vMac's CPU so both see the
 * same I/O. Both execute identical instructions until a real CPU bug. */
static unsigned char g_devq[64];
static int g_devq_head, g_devq_tail;

static void devq_log(mac_mem *m, u32 addr, u32 val, int is_write, int size) {
    (void)m; (void)size;
    if (is_write) return;
    if (addr < 0x500000u) return;             /* RAM / ROM — not a device */
    if (((g_devq_tail + 1) & 63) == g_devq_head) return;
    g_devq[g_devq_tail] = (unsigned char)val;
    g_devq_tail = (g_devq_tail + 1) & 63;
}

/* --- RAM-write tracking ----------------------------------------------
 * Comparing the whole 1 MB of RAM after every instruction is far too
 * slow to run Speedometer's benchmarks through the differential. Instead
 * record the RAM offsets this project's CPU writes (via mac_write_watch)
 * and compare only those — O(writes) instead of O(ram_size). A periodic
 * full sweep is the backstop for the rare mini-vMac-only write. */
static u32  g_woff[256];
static int  g_woff_n;
static bool g_woff_of;            /* write buffer overflowed this step */

static void woff_log(void *ctx, u32 ra) {
    (void)ctx;
    if (g_woff_n < 256) g_woff[g_woff_n++] = ra;
    else g_woff_of = true;
}

unsigned mvdiff_dev_read(unsigned addr, int is_word) {
    (void)addr;
    unsigned v = 0;
    for (int i = 0; i <= is_word; i++) {
        unsigned b = 0;
        if (g_devq_head != g_devq_tail) {
            b = g_devq[g_devq_head];
            g_devq_head = (g_devq_head + 1) & 63;
        }
        v = (v << 8) | b;
    }
    return v;
}

/* mini vMac core, via tools/minem_glue.c */
void     MNVM_init(unsigned char *, unsigned, unsigned char *, unsigned);
void     MNVM_set_state(const unsigned[16], unsigned, unsigned, unsigned,
                        unsigned);
void     MNVM_get_regs(unsigned[16]);
unsigned MNVM_getpc(void);
unsigned MNVM_getSR(void);
void     MNVM_step(void);

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: mvmac_diff <snapshot> "
                                    "[max-steps]\n"); return 1; }
    long max_steps = argc > 2 ? atol(argv[2]) : 20000000;

    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror("snapshot"); return 1; }
    unsigned hdr[24];
    if (fread(hdr, 4, 24, f) != 24 || hdr[0] != 0x4D414331u) {
        fprintf(stderr, "bad snapshot\n"); return 1;
    }
    unsigned ram_size, rom_size;
    fread(&ram_size, 4, 1, f);
    unsigned char *ram_snap = malloc(ram_size);
    fread(ram_snap, 1, ram_size, f);
    fread(&rom_size, 4, 1, f);
    unsigned char *rom_snap = malloc(rom_size);
    fread(rom_snap, 1, rom_size, f);
    fclose(f);
    fprintf(stderr, "snapshot: pc=%06X sr=%04X ram=%uK rom=%uK\n",
            hdr[17], hdr[18], ram_size >> 10, rom_size >> 10);

    /* --- this project's interpreter --- */
    mac_mem mA;
    mac_mem_init(&mA, ram_size);
    memcpy(mA.ram, ram_snap, ram_size);
    free(mA.rom);
    mA.rom = malloc(rom_size);
    memcpy(mA.rom, rom_snap, rom_size);
    mA.rom_size = rom_size;
    mA.overlay = false;
    m68k_cpu cA;
    memset(&cA, 0, sizeof(cA));
    for (int i = 0; i < 8; i++) { cA.d[i] = hdr[1 + i];
                                  cA.a[i] = hdr[9 + i]; }
    cA.pc = hdr[17]; cA.sr = hdr[18]; cA.usp = hdr[19]; cA.ssp = hdr[20];
    cA.mem = &mA;
    mA.cpu = &cA;
    mac_mmio_log = devq_log;          /* capture this CPU's device reads */
    mac_write_watch = woff_log;       /* capture this CPU's RAM writes  */

    /* --- mini vMac CPU --- */
    unsigned char *ramB = malloc(ram_size); memcpy(ramB, ram_snap, ram_size);
    unsigned char *romB = malloc(rom_size); memcpy(romB, rom_snap, rom_size);
    MNVM_init(ramB, ram_size - 1, romB, rom_size - 1);
    unsigned r16[16];
    for (int i = 0; i < 8; i++) { r16[i] = hdr[1 + i]; r16[8 + i] = hdr[9 + i]; }
    MNVM_set_state(r16, hdr[17], hdr[18], hdr[19], hdr[20]);

    /* sanity: both must agree on the starting PC */
    if (MNVM_getpc() != cA.pc)
        fprintf(stderr, "warn: start PC mismatch mine=%06X mvmac=%06X\n",
                cA.pc, MNVM_getpc());

    for (long step = 0; step < max_steps; step++) {
        unsigned pc_before = cA.pc;
        unsigned op = mac_read16(&mA, pc_before);

        g_devq_head = g_devq_tail = 0;     /* fresh per instruction */
        g_woff_n = 0; g_woff_of = false;
        m68k_step(&cA);                    /* records device reads + writes */
        MNVM_step();                       /* replays them         */

        unsigned rB[16];
        MNVM_get_regs(rB);
        unsigned pcB = MNVM_getpc();

        unsigned srB = MNVM_getSR();
        int bad = 0;
        for (int i = 0; i < 8 && !bad; i++) if (cA.d[i] != rB[i]) bad = 1;
        for (int i = 0; i < 8 && !bad; i++) if (cA.a[i] != rB[8 + i]) bad = 1;
        if (cA.pc != pcB) bad = 1;
        if ((cA.sr & 0x1F) != (srB & 0x1F)) bad = 1;   /* condition codes */

        /* memory divergence — the instruction just executed stored a
         * different value (registers can still match). Check only the
         * bytes this CPU wrote; sweep all of RAM periodically as a
         * backstop, and whenever the write buffer overflowed. */
        if (!bad) {
            long md = -1;
            if (g_woff_of || (step & 0x3FFFF) == 0) {
                for (unsigned a = 0; a < ram_size; a++)
                    if (mA.ram[a] != ramB[a]) { md = (long)a; break; }
            } else {
                for (int i = 0; i < g_woff_n && md < 0; i++) {
                    u32 a = g_woff[i];
                    if (a < ram_size && mA.ram[a] != ramB[a]) md = (long)a;
                }
            }
            if (md >= 0) {
                printf("\n*** MEMORY DIVERGENCE at step %ld ***\n", step);
                printf("instruction: PC=%06X  opcode=%04X\n",
                       pc_before, op);
                printf("  RAM[%06lX]: this project=%02X  mini vMac=%02X\n",
                       md, mA.ram[md], ramB[md]);
                for (int i = 0; i < 8; i++)
                    printf("  D%d=%08X  A%d=%08X\n", i, cA.d[i], i, cA.a[i]);
                printf("  PC=%06X  SR=%04X  this CPU wrote %d byte(s):",
                       cA.pc, cA.sr & 0xFFFF, g_woff_n);
                for (int i = 0; i < g_woff_n; i++)
                    printf(" %06X", g_woff[i]);
                printf("\n");
                return 3;
            }
        }

        if (bad) {
            printf("\n*** DIVERGENCE at step %ld ***\n", step);
            printf("instruction: PC=%06X  opcode=%04X\n", pc_before, op);
            printf("            %-12s %-12s\n", "this project", "mini vMac");
            for (int i = 0; i < 8; i++)
                printf("  D%d        %08X     %08X%s\n", i, cA.d[i], rB[i],
                       cA.d[i] != rB[i] ? "   <<<" : "");
            for (int i = 0; i < 8; i++)
                printf("  A%d        %08X     %08X%s\n", i, cA.a[i],
                       rB[8 + i], cA.a[i] != rB[8 + i] ? "   <<<" : "");
            printf("  PC        %08X     %08X%s\n", cA.pc, pcB,
                   cA.pc != pcB ? "   <<<" : "");
            printf("  SR        %04X         %04X\n",
                   cA.sr & 0xFFFF, MNVM_getSR());
            return 2;
        }
        if (cA.halted) {
            printf("halted after %ld steps, no divergence\n", step);
            return 0;
        }
    }
    printf("no divergence in %ld steps\n", max_steps);
    return 0;
}
