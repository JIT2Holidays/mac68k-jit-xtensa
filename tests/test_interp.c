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

int main(void) {
    if (test_arith()) return fail("arith snippet");

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
