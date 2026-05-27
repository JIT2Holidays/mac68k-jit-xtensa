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

/* Run a 1-instruction encoding (with optional ext words) under the
 * interpreter, starting at PC=0x100 in SR=supervisor with the given
 * pre-state. Returns the cpu after the single step. */
static void run_one(mac_mem *mem, m68k_cpu *cpu, const u16 *enc, int n_words,
                    u32 d_in[8], u32 a_in[8], u8 ccr_in) {
    for (int i = 0; i < n_words; i++)
        mac_write16(mem, 0x100 + (u32)i * 2, enc[i]);
    memset(cpu, 0, sizeof(*cpu));
    cpu->mem = mem; mem->cpu = cpu;
    cpu->pc = 0x100;
    cpu->sr = (u16)(SR_S | ccr_in);
    if (d_in) memcpy(cpu->d, d_in, sizeof cpu->d);
    if (a_in) memcpy(cpu->a, a_in, sizeof cpu->a);
    else      cpu->a[7] = 0x4000;
    m68k_step(cpu);
}

/* 68020/030 ISA additions — landed in M7.0 alongside the SE/30 machine
 * model. Each test exercises one new opcode end-to-end through the
 * reference interpreter. */
static int test_68030_isa(void) {
    mac_mem mem;
    mac_mem_init(&mem, 64 * 1024);
    mem.overlay = false;
    m68k_cpu cpu;
    int rc = 0;

    /* BFEXTU D0{4:8}, D1 — extract 8 bits starting at bit offset 4 from
     * D0 into D1. With D0 = 0xABCD1234, the field at bits [4..11] of the
     * big-endian view (bits 27..20 of the LE register, since 68k counts
     * bit-offset from MSB) is the byte 0xBC. */
    {
        u16 enc[] = { 0xE9C0, (u16)((1 << 12) | (4 << 6) | 8) };
        u32 d[8] = { 0xABCD1234, 0, 0, 0, 0, 0, 0, 0 };
        run_one(&mem, &cpu, enc, 2, d, NULL, 0);
        if (cpu.d[1] != 0xBC) {
            printf("BFEXTU: D1=%08X want 000000BC\n", cpu.d[1]); rc = 1;
        }
    }
    /* MULS.L #-3, D0 with D0 = 7 -> -21 (0xFFFFFFEB). Use Dq=0, Dr=0
     * (32-bit product); src EA is #imm.L. */
    {
        u16 enc[] = { 0x4C3C,                  /* MULS.L (Dq form), <ea> #imm */
                       (u16)((0 << 12) | (1 << 11) | (0 << 10)),  /* dq=0, signed, 32-bit */
                       0xFFFF, 0xFFFD };       /* #-3 .L */
        u32 d[8] = { 7, 0, 0, 0, 0, 0, 0, 0 };
        run_one(&mem, &cpu, enc, 4, d, NULL, 0);
        if (cpu.d[0] != 0xFFFFFFEB) {
            printf("MULS.L: D0=%08X want FFFFFFEB\n", cpu.d[0]); rc = 1;
        }
    }
    /* DIVS.L #7, D0 with D0 = -21 -> -3 (0xFFFFFFFD). */
    {
        u16 enc[] = { 0x4C3C,
                       (u16)((0 << 12) | (1 << 11) | (0 << 10) | 0),
                       0x0000, 0x0007 };
        /* That's MUL not DIV — need 0x4C7C for DIV. */
        enc[0] = 0x4C7C;                      /* DIVS.L #imm, Dq */
        u32 d[8] = { 0xFFFFFFEBu, 0, 0, 0, 0, 0, 0, 0 };
        run_one(&mem, &cpu, enc, 4, d, NULL, 0);
        if (cpu.d[0] != 0xFFFFFFFD) {
            printf("DIVS.L: D0=%08X want FFFFFFFD\n", cpu.d[0]); rc = 1;
        }
    }
    /* EXTB.L D0 with D0 = 0x12345680 -> 0xFFFFFF80. */
    {
        u16 enc[] = { 0x49C0 };
        u32 d[8] = { 0x12345680, 0, 0, 0, 0, 0, 0, 0 };
        run_one(&mem, &cpu, enc, 1, d, NULL, 0);
        if (cpu.d[0] != 0xFFFFFF80) {
            printf("EXTB.L: D0=%08X want FFFFFF80\n", cpu.d[0]); rc = 1;
        }
    }
    /* MOVEC #imm, VBR — write 0x1000 to VBR via D0. Then MOVEC VBR, D1
     * reads it back. */
    {
        u16 enc[] = { 0x4E7B, (u16)((0 << 12) | 0x801) };   /* MOVEC D0, VBR */
        u32 d[8] = { 0x1000, 0, 0, 0, 0, 0, 0, 0 };
        run_one(&mem, &cpu, enc, 2, d, NULL, 0);
        if (cpu.vbr != 0x1000) {
            printf("MOVEC->VBR: VBR=%08X want 1000\n", cpu.vbr); rc = 1;
        }
    }
    /* RTD #d16: pop PC, then SP += 4 + d16. SP=0x1000, mem[0x1000] = 0x2000. */
    {
        mac_write32(&mem, 0x1000, 0x00002000);
        u16 enc[] = { 0x4E74, 0x0008 };
        u32 a[8] = { 0, 0, 0, 0, 0, 0, 0, 0x1000 };
        run_one(&mem, &cpu, enc, 2, NULL, a, 0);
        if (cpu.pc != 0x2000) { printf("RTD: PC=%08X want 2000\n", cpu.pc); rc = 1; }
        if (cpu.a[7] != 0x100C) { printf("RTD: SP=%08X want 100C\n", cpu.a[7]); rc = 1; }
    }
    /* LINK.L A6, #-0x10000: push A6, set A6=SP, SP += -0x10000. */
    {
        u16 enc[] = { 0x480E, 0xFFFF, 0x0000 };
        u32 a[8] = { 0, 0, 0, 0, 0, 0, 0xDEADBEEF, 0x4000 };
        run_one(&mem, &cpu, enc, 3, NULL, a, 0);
        if (cpu.a[6] != 0x3FFC) { printf("LINK.L: A6=%08X want 3FFC\n", cpu.a[6]); rc = 1; }
        if (cpu.a[7] != 0xFFFF3FFC) { printf("LINK.L: SP=%08X want FFFF3FFC\n", cpu.a[7]); rc = 1; }
    }
    /* TRAPcc with cc=F (op == 0x57FC for `TRAPF`): never traps. */
    {
        u16 enc[] = { 0x51FC };               /* TRAPF (cc=False) — never traps */
        run_one(&mem, &cpu, enc, 1, NULL, NULL, 0);
        if (cpu.pc != 0x102) {
            printf("TRAPF: PC=%08X want 102 (no trap)\n", cpu.pc); rc = 1;
        }
    }
    /* PACK Dn,Dn: ('5' << 8 | '7') + 0xFF00 packed -> '57' (0x57). */
    {
        u16 enc[] = { 0x8141, 0x0000 };       /* PACK D1,D0,#0 — adj=0 */
        u32 d[8] = { 0, 0x3537, 0, 0, 0, 0, 0, 0 };
        run_one(&mem, &cpu, enc, 2, d, NULL, 0);
        if ((cpu.d[0] & 0xFF) != 0x57) {
            printf("PACK: D0=%02X want 57\n", cpu.d[0] & 0xFF); rc = 1;
        }
    }
    mac_mem_free(&mem);
    if (!rc) printf("  68020/030 ISA additions OK\n");
    return rc;
}

/* SE/30 memory map init must succeed and route region decode correctly. */
static int test_se30_init_stable(void) {
    mac_mem mem;
    mac_mem_init_ex(&mem, MAC_MODEL_SE30, 8u * 1024u * 1024u);
    if (mem.model != MAC_MODEL_SE30) {
        printf("se30: model = %d want 1\n", mem.model); return 1;
    }
    /* SE/30 reset state has the boot overlay on (ROM mirrored at low
     * addresses). It's flipped off by the ROM writing to the Glue
     * register at 0x5FFFFFFE during init. */
    if (!mem.overlay) {
        printf("se30: overlay clear at init, expected on\n"); return 1;
    }
    /* Trigger overlay-off by writing the Glue register, then verify. */
    mac_write8(&mem, 0x5FFFFFFEu, 0);
    if (mem.overlay) {
        printf("se30: overlay still on after Glue write\n"); return 1;
    }
    /* Stub ASC/ADB present in the struct (no crash on access). */
    mac_write8(&mem, MAC_SE30_ASC_BASE + 0x10, 0x55);
    if (mac_read8(&mem, MAC_SE30_ASC_BASE + 0x10) != 0x55) {
        printf("se30: ASC register stub didn't round-trip\n"); return 1;
    }
    /* VIA2 register stub round-trip. */
    mac_write8(&mem, MAC_SE30_VIA2_BASE + (11 << 9), 0x42);   /* ACR */
    if (mac_read8(&mem, MAC_SE30_VIA2_BASE + (11 << 9)) != 0x42) {
        printf("se30: VIA2 ACR didn't round-trip\n"); return 1;
    }
    /* 32-bit RAM access above 0x1000000 (16 MB). */
    mac_write32(&mem, 0x00100000, 0xDEADBEEF);
    if (mac_read32(&mem, 0x00100000) != 0xDEADBEEF) {
        printf("se30: high-RAM write didn't stick\n"); return 1;
    }
    mac_mem_free(&mem);
    printf("  SE/30 init + region decode OK\n");
    return 0;
}

/* M7.6a — PMMU translation framework unit test.
 * Verifies pass-through behaviour, TC.E gate, and TT0/TT1 transparent
 * translation match logic. Real PTW is TODO. */
static int test_pmmu_translate(void) {
    mac_mem mem;
    mac_mem_init_ex(&mem, MAC_MODEL_SE30, 1024 * 1024);
    m68k_cpu cpu;
    m68k_reset(&cpu, &mem);

    /* Plus mode short-circuit (different mac_mem instance). */
    mac_mem plus;
    mac_mem_init_ex(&plus, MAC_MODEL_PLUS, 64 * 1024);
    m68k_cpu pcpu;
    m68k_reset(&pcpu, &plus);
    if (mac_pmmu_translate(&plus, 0x12345678u, 5) != 0x12345678u) {
        printf("pmmu: Plus translate not pass-through\n"); return 1;
    }
    mac_mem_free(&plus);

    /* SE/30 with TC.E = 0 → pass-through. */
    cpu.tc = 0;
    if (mac_pmmu_translate(&mem, 0x12345678u, 5) != 0x12345678u) {
        printf("pmmu: TC.E=0 not pass-through\n"); return 1;
    }

    /* TC.E = 1, no TT0/TT1 set → still pass-through (TODO real PTW). */
    cpu.tc = 0x80000000u;
    cpu.tt0 = 0;
    cpu.tt1 = 0;
    if (mac_pmmu_translate(&mem, 0x12345678u, 5) != 0x12345678u) {
        printf("pmmu: TC.E=1 no-TT path not pass-through\n"); return 1;
    }

    /* TT0 enabled covering all FCs (mask=0xF) and LA base 0x12 with mask
     * 0xFF → matches any address starting with 0x12. */
    cpu.tt0 = 0x12FF8000u | 0xFu;  /* LA base 0x12, LA mask 0xFF, E=1, FC mask 0xF */
    if (mac_pmmu_translate(&mem, 0x12345678u, 5) != 0x12345678u) {
        printf("pmmu: TT0 match not pass-through\n"); return 1;
    }

    mac_mem_free(&mem);
    (void)cpu; (void)pcpu;
    printf("  PMMU translate framework OK (pass-through paths)\n");
    return 0;
}

int main(void) {
    if (test_arith()) return fail("arith snippet");
    if (test_xform_flags()) return fail("X-form condition codes");
    if (test_68030_isa()) return fail("68030 ISA");
    if (test_se30_init_stable()) return fail("SE/30 init");
    if (test_pmmu_translate()) return fail("PMMU framework");

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
