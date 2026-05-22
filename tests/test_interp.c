/* Interpreter smoke test: the built-in demo must run to a clean exit. */

#include "m68k_cpu.h"
#include "m68k_interp.h"
#include "mac_mem.h"
#include "demo_rom.h"
#include "m68k_asm.h"
#include <stdio.h>
#include <string.h>

static char serial[256];
static int  serial_len;
static void sink(void *ctx, u8 b) {
    (void)ctx;
    if (serial_len < (int)sizeof(serial) - 1) serial[serial_len++] = (char)b;
}

static int fail(const char *m) { printf("FAIL: %s\n", m); return 1; }

/* A hand-built snippet exercising arithmetic + flags + a branch. */
static int test_arith(void) {
    mac_mem mem;
    mac_mem_init(&mem, 256 * 1024);
    u8 img[512];
    m68a a;
    m68a_init(&a, img, sizeof(img), 0);
    m68a_w32(&a, 0x00020000);            /* SSP */
    m68a_w32(&a, 0x00000100);            /* PC  */
    while (m68a_here(&a) < 0x100) m68a_w16(&a, 0x4E71);
    int loop = m68a_label(&a), end = m68a_label(&a);
    m68a_moveq(&a, 0, 0);                /* D0 = 0 */
    m68a_moveq(&a, 1, 10);               /* D1 = 10 */
    m68a_mark(&a, loop);
    m68a_add_l(&a, 0, 1);                /* D0 += D1 */
    m68a_subq_l(&a, 1, 1);               /* D1 -= 1  */
    m68a_bne(&a, loop);
    m68a_mark(&a, end);
    m68a_stop(&a, 0x2700);
    m68a_finish(&a);
    mac_load_ram_image(&mem, 0, img, a.len);

    m68k_cpu cpu;
    m68k_reset(&cpu, &mem);
    m68k_run_until(&cpu, 100000);
    mac_mem_free(&mem);

    if (cpu.d[0] != 55) { printf("arith: D0=%u want 55\n", cpu.d[0]); return 1; }
    if (cpu.d[1] != 0)  { printf("arith: D1=%u want 0\n", cpu.d[1]); return 1; }
    printf("  arith snippet: D0=55 OK\n");
    return 0;
}

/* ADDX/SUBX/NEGX condition codes. These X-form ("extended") ops differ
 * from plain ADD/SUB/NEG in three ways the mini vMac differential caught
 * while running Speedometer's benchmarks:
 *   1. Z is sticky — cleared on a nonzero result, left UNCHANGED on a
 *      zero result (so Z stays valid across a multi-precision op).
 *   2. carry/borrow (and hence X) must include the X input.
 *   3. NEGX must compute V (overflow), which it was omitting entirely.
 * Each case below checks the full 5-bit CCR after one instruction. */
static int test_xform_flags(void) {
    mac_mem mem;
    mac_mem_init(&mem, 64 * 1024);
    mem.overlay = false;                  /* plain RAM at low addresses */
    mac_write16(&mem, 0x100, 0xD182);     /* ADDX.L D2,D0 */
    mac_write16(&mem, 0x102, 0x9182);     /* SUBX.L D2,D0 */
    mac_write16(&mem, 0x104, 0x4080);     /* NEGX.L D0    */
    m68k_cpu cpu;
    int rc = 0;

    struct { const char *name; u16 pc; u32 d0, d2; u8 ccr_in, ccr_out; } cs[] = {
        /* sticky Z: zero result keeps the incoming Z either way */
        { "ADDX 0+0,    Z=1",   0x100, 0, 0, CCR_Z,         CCR_Z },
        { "ADDX 0+0,    Z=0",   0x100, 0, 0, 0,             0 },
        { "ADDX nonzero,Z=1",   0x100, 1, 0, CCR_Z,         0 },
        /* carry must include X: 0xFFFFFFFF+0+X wraps to 0, sets C|X */
        { "ADDX FFFF.+X,Z=1",   0x100, 0xFFFFFFFF, 0, CCR_X|CCR_Z,
                                                 CCR_Z|CCR_C|CCR_X },
        /* borrow must include X: 0-0-X = -1, borrow sets C|X, N set */
        { "SUBX 0-0-X",         0x102, 0, 0, CCR_X,         CCR_N|CCR_C|CCR_X },
        { "SUBX 0-0,    Z=1",   0x102, 0, 0, CCR_Z,         CCR_Z },
        /* NEGX of the most-negative value overflows: V must be set */
        { "NEGX 80000000",      0x104, 0x80000000, 0, 0,
                                            CCR_N|CCR_V|CCR_C|CCR_X },
        { "NEGX 0,      Z=1",   0x104, 0, 0, CCR_Z,         CCR_Z },
    };
    for (int i = 0; i < (int)(sizeof cs / sizeof cs[0]); i++) {
        memset(&cpu, 0, sizeof cpu);
        cpu.mem = &mem; mem.cpu = &cpu;
        cpu.pc = cs[i].pc; cpu.d[0] = cs[i].d0; cpu.d[2] = cs[i].d2;
        cpu.sr = (u16)(SR_S | cs[i].ccr_in);
        m68k_step(&cpu);
        u8 got = (u8)(cpu.sr & 0x1F);
        if (got != cs[i].ccr_out) {
            printf("xform-flags: %s -> CCR=%02X want %02X\n",
                   cs[i].name, got, cs[i].ccr_out);
            rc = 1;
        }
    }
    mac_mem_free(&mem);
    if (!rc) printf("  ADDX/SUBX/NEGX condition codes OK\n");
    return rc;
}

int main(void) {
    if (test_arith()) return fail("arith snippet");
    if (test_xform_flags()) return fail("X-form condition codes");

    mac_mem mem;
    mac_mem_init(&mem, 1024 * 1024);
    mem.serial_sink = sink;

    u8 img[DEMO_ROM_MAX];
    u32 len = demo_rom_build(img, mem.fb_base);
    mac_load_ram_image(&mem, 0, img, len);

    m68k_cpu cpu;
    m68k_reset(&cpu, &mem);
    m68k_run_until(&cpu, 50000000ull);

    printf("  demo: halted=%d exit=%d cycles=%llu serial=\"%.*s\"\n",
           cpu.halted, cpu.exit_code, (unsigned long long)cpu.cycles,
           serial_len, serial);

    if (!cpu.halted)                return fail("demo did not halt");
    if (cpu.exit_code != 0)         return fail("demo exit code != 0");
    if (!strstr(serial, "PASS"))    return fail("demo did not print PASS");

    /* The framebuffer fill must have landed. */
    if (mac_read32(&mem, mem.fb_base) != 0xA5A5A5A5u)
        return fail("framebuffer not filled");

    mac_mem_free(&mem);
    printf("PASS: interpreter\n");
    return 0;
}
