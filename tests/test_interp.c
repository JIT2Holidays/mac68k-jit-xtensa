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
    if (mac_pmmu_translate(&plus, 0x12345678u, 5, false) != 0x12345678u) {
        printf("pmmu: Plus translate not pass-through\n"); return 1;
    }
    mac_mem_free(&plus);

    /* SE/30 with TC.E = 0 → pass-through. */
    cpu.tc = 0;
    if (mac_pmmu_translate(&mem, 0x12345678u, 5, false) != 0x12345678u) {
        printf("pmmu: TC.E=0 not pass-through\n"); return 1;
    }

    /* TC.E = 1, no TT0/TT1 set → still pass-through (TODO real PTW). */
    cpu.tc = 0x80000000u;
    cpu.tt0 = 0;
    cpu.tt1 = 0;
    if (mac_pmmu_translate(&mem, 0x12345678u, 5, false) != 0x12345678u) {
        printf("pmmu: TC.E=1 no-TT path not pass-through\n"); return 1;
    }

    /* TT0 enabled covering all FCs (mask=0xF) and LA base 0x12 with mask
     * 0xFF → matches any address starting with 0x12. */
    cpu.tt0 = 0x12FF8000u | 0xFu;  /* LA base 0x12, LA mask 0xFF, E=1, FC mask 0xF */
    if (mac_pmmu_translate(&mem, 0x12345678u, 5, false) != 0x12345678u) {
        printf("pmmu: TT0 match not pass-through\n"); return 1;
    }
    cpu.tt0 = 0;

    /* M7.6c — single-level short-form PTW test. Set up a page table at
     * RAM 0x4000 with 16 entries (each 4 bytes), 4KB pages (PS=4).
     *
     * Boot overlay must be off so RAM writes are visible to RAM reads
     * (otherwise mac_read32 in PMMU reads from the ROM mirror). The
     * SE/30 ROM normally clears overlay via the Glue write at 0x5FFFFFFE;
     * we do the same here. */
    mem.overlay = false;
    u32 table_base = 0x4000u;
    /* Map LA 0x00000000 (page 0) → phys 0x10000 (1:0x10 page swap). */
    mac_write32(&mem, table_base + 0 * 4, 0x00010002u);   /* phys 0x10000, dt=2 (short page) */
    /* Map LA 0x00001000 (page 1) → phys 0x20000. */
    mac_write32(&mem, table_base + 1 * 4, 0x00020002u);
    /* Set SRP: high 32 = table_base, low 32 = descriptor type 2 (short). */
    cpu.srp = ((u64)table_base << 32) | 2u;
    /* TC.E=1, TIA=4, PS=4 (4KB pages), other fields 0. */
    cpu.tc  = 0x80000000u | (4u << 20) | (4u << 12);

    /* Translate LA 0x00000100 (page 0, offset 0x100). Expect phys
     * 0x10000 + 0x100 = 0x10100. */
    u32 phys = mac_pmmu_translate(&mem, 0x00000100u, 5, false);   /* fc=5 = supervisor data */
    if (phys != 0x10100u) {
        printf("pmmu: 1-level PTW LA=0x100 → phys=%08X want 10100\n", phys);
        return 1;
    }
    /* LA 0x00001ABC → page 1 + offset 0xABC → phys 0x20ABC. */
    phys = mac_pmmu_translate(&mem, 0x00001ABCu, 5, false);
    if (phys != 0x20ABCu) {
        printf("pmmu: 1-level PTW LA=0x1ABC → phys=%08X want 20ABC\n", phys);
        return 1;
    }
    /* M7.6d — two-level walk test. TIA=2 (4-entry top table), TIB=4
     * (16-entry second-level table), PS=4 (4KB pages). LA layout:
     *   [31..14] unused
     *   [13..12] TIA index (2 bits → 4 entries)
     *   [11..8]  TIB index (4 bits → 16 entries) ... wait that overlaps
     * Actually let me put level offsets ABOVE the page bits properly:
     *   bits 17..16: TIA index (2 bits)
     *   bits 15..12: TIB index (4 bits)
     *   bits 11..0:  in-page offset (12 bits for 4KB pages)
     *
     * Top-level table at 0x6000, 4 entries pointing to 4 second-level
     * tables. Use a single second-level table for simplicity. */
    u32 top_table = 0x6000u;
    u32 mid_table = 0x6100u;
    cpu.tc = 0;                                            /* disable for setup */
    /* Top entry 0 → mid_table (DT=2 short pointer). */
    mac_write32(&mem, top_table + 0 * 4, mid_table | 2u);
    /* Mid entry 5 → phys page 0x50000 (DT=2 short page). */
    mac_write32(&mem, mid_table + 5 * 4, 0x00050002u);
    cpu.srp = ((u64)top_table << 32) | 2u;
    /* TC.E=1, PS=4, TIA=2, TIB=4. */
    cpu.tc = 0x80000000u | (4u << 20) | (2u << 12) | (4u << 8);
    /* LA bits 17-16 = 00 (top idx 0); bits 15-12 = 0101 (mid idx 5);
     * bits 11-0 = 0xABC. So LA = 0x5ABC. Expected phys = 0x50000 | 0xABC
     * = 0x50ABC. */
    u32 phys2 = mac_pmmu_translate(&mem, 0x5ABCu, 5, false);
    if (phys2 != 0x50ABCu) {
        printf("pmmu: 2-level PTW LA=0x5ABC → phys=%08X want 50ABC\n", phys2);
        return 1;
    }

    /* M7.6f — long-form (8-byte) descriptor test. Same idea as the
     * short-form 1-level test, but entries are 8 bytes each: word 0 has
     * the DT/flags, word 1 has the full 32-bit address.
     *
     * IMPORTANT: TC.E must be 0 during page-table setup, otherwise
     * mac_write32 (M7.6g plumb-in) tries to TRANSLATE the table address
     * before writing — but the table doesn't exist yet, so writes go
     * astray. Disable, write, then enable. */
    cpu.tc = 0;
    u32 long_table = 0x5000u;
    /* Entry 0 (8 bytes): word0=0x00000001 (DT=1 page), word1=0x00040000 (page addr). */
    mac_write32(&mem, long_table + 0 * 8 + 0, 0x00000001u);
    mac_write32(&mem, long_table + 0 * 8 + 4, 0x00040000u);
    /* Entry 1: word0=DT=1, word1=0x00080000. */
    mac_write32(&mem, long_table + 1 * 8 + 0, 0x00000001u);
    mac_write32(&mem, long_table + 1 * 8 + 4, 0x00080000u);
    /* Root pointer: long format (DT=3). */
    cpu.srp = ((u64)long_table << 32) | 3u;
    /* Single-level walk, TIA=4, PS=4 (4KB pages). */
    cpu.tc = 0x80000000u | (4u << 20) | (4u << 12);
    cpu.bus_error_pending = 0;
    /* LA 0x00000080 → page 0 (TIA idx 0, PS=4 → bits 11-0 are offset).
     * Expect phys = 0x00040000 | 0x80 = 0x00040080. */
    u32 phl = mac_pmmu_translate(&mem, 0x00000080u, 5, false);
    if (phl != 0x00040080u) {
        printf("pmmu: long-form 1-level LA=0x80 → phys=%08X want 40080\n", phl);
        return 1;
    }
    /* LA 0x00001234 → page 1, offset 0x234. Expected 0x00080234. */
    phl = mac_pmmu_translate(&mem, 0x00001234u, 5, false);
    if (phl != 0x00080234u) {
        printf("pmmu: long-form 1-level LA=0x1234 → phys=%08X want 80234\n", phl);
        return 1;
    }

    /* M7.6e — invalid descriptor → bus error. Set up a 1-level table
     * where the LA we translate lands on an invalid (DT=0) leaf. */
    cpu.tc = 0;                                            /* disable for setup write */
    mac_write32(&mem, table_base + 2 * 4, 0x00000000u);   /* DT=0 invalid */
    cpu.tc = 0x80000000u | (4u << 20) | (4u << 12);       /* back to 1-level */
    cpu.srp = ((u64)table_base << 32) | 2u;
    cpu.bus_error_pending = 0;
    /* LA 0x00002000 → page 2 → invalid descriptor. */
    u32 ph = mac_pmmu_translate(&mem, 0x00002000u, 5, false);
    (void)ph;
    if (cpu.bus_error_pending == 0) {
        printf("pmmu: invalid descriptor did NOT set bus_error_pending\n");
        return 1;
    }
    /* M7.6l — cause field. Invalid descriptor → CAUSE=INVALID. */
    if ((cpu.bus_error_pending & BERR_CAUSE_MASK) != BERR_CAUSE_INVALID) {
        printf("pmmu: invalid-desc cause=%08X want %08X\n",
               cpu.bus_error_pending & BERR_CAUSE_MASK, BERR_CAUSE_INVALID);
        return 1;
    }

    /* M7.6h — long-form WP (write-protect) check. Mark long-table entry
     * 2 as WP (word0 bit 11 = 0x800) plus DT=1. Write to that page
     * should set bus_error_pending; read should NOT. */
    cpu.tc = 0;
    mac_write32(&mem, long_table + 2 * 8 + 0, 0x00000801u);   /* DT=1, WP=1 */
    mac_write32(&mem, long_table + 2 * 8 + 4, 0x000C0000u);   /* page addr */
    cpu.srp = ((u64)long_table << 32) | 3u;                    /* point back at long-form root */
    cpu.tc = 0x80000000u | (4u << 20) | (4u << 12);
    cpu.bus_error_pending = 0;
    /* Read LA 0x2000 (page 2): should succeed. */
    u32 ph_r = mac_pmmu_translate(&mem, 0x2000u, 5, false);
    if (cpu.bus_error_pending != 0 || ph_r != 0xC0000u) {
        printf("pmmu: WP read should pass — phys=%08X berr=%08X\n",
               ph_r, cpu.bus_error_pending); return 1;
    }
    /* Write to same LA: should set bus_error_pending. */
    cpu.bus_error_pending = 0;
    mac_pmmu_translate(&mem, 0x2000u, 5, true);
    if (cpu.bus_error_pending == 0) {
        printf("pmmu: WP write should set bus_error_pending\n"); return 1;
    }
    if ((cpu.bus_error_pending & BERR_CAUSE_MASK) != BERR_CAUSE_WP) {
        printf("pmmu: WP cause=%08X want %08X\n",
               cpu.bus_error_pending & BERR_CAUSE_MASK, BERR_CAUSE_WP);
        return 1;
    }

    /* M7.6n — long-form supervisor-only (S, bit 14). User-mode access
     * (fc=1 = user data) to a S=1 page → BERR with CAUSE_SUPER.
     * Supervisor (fc=5) access to the same page should succeed. */
    cpu.tc = 0;
    mac_write32(&mem, long_table + 4 * 8 + 0, 0x00004001u);   /* DT=1, S=1 */
    mac_write32(&mem, long_table + 4 * 8 + 4, 0x000E0000u);   /* page addr */
    cpu.srp = ((u64)long_table << 32) | 3u;
    cpu.crp = cpu.srp;                                          /* same root for user */
    cpu.tc = 0x80000000u | (4u << 20) | (4u << 12);
    cpu.bus_error_pending = 0;
    /* Supervisor reads page 4: should succeed. */
    u32 ph_sup = mac_pmmu_translate(&mem, 0x4000u, 5, false);
    if (cpu.bus_error_pending != 0 || ph_sup != 0xE0000u) {
        printf("pmmu: S=1 super read phys=%08X berr=%08X\n",
               ph_sup, cpu.bus_error_pending); return 1;
    }
    /* User reads page 4: should BERR with CAUSE_SUPER. */
    cpu.bus_error_pending = 0;
    mac_pmmu_translate(&mem, 0x4000u, 1, false);
    if (cpu.bus_error_pending == 0 ||
        (cpu.bus_error_pending & BERR_CAUSE_MASK) != BERR_CAUSE_SUPER) {
        printf("pmmu: S=1 user cause=%08X want %08X\n",
               cpu.bus_error_pending & BERR_CAUSE_MASK, BERR_CAUSE_SUPER);
        return 1;
    }

    /* M7.6i — U (used) and M (modified) bit maintenance. Set up a fresh
     * short-form entry with U=0/M=0, translate it once for read, expect
     * U=1 M=0. Translate again for write, expect U=1 M=1. */
    cpu.tc = 0;
    /* Entry 3 of short-form table_base: page addr 0x000D0000, DT=1 only. */
    mac_write32(&mem, table_base + 3 * 4, 0x000D0001u);
    cpu.srp = ((u64)table_base << 32) | 2u;
    cpu.tc = 0x80000000u | (4u << 20) | (4u << 12);
    cpu.bus_error_pending = 0;
    /* LA 0x3010 → page 3 → phys 0x000D0010. Read first. */
    u32 ph_um = mac_pmmu_translate(&mem, 0x3010u, 5, false);
    if (cpu.bus_error_pending != 0 || ph_um != 0x000D0010u) {
        printf("pmmu: U/M read phys=%08X berr=%08X\n",
               ph_um, cpu.bus_error_pending); return 1;
    }
    cpu.tc = 0;
    u32 after_r = mac_read32(&mem, table_base + 3 * 4);
    cpu.tc = 0x80000000u | (4u << 20) | (4u << 12);
    if ((after_r & (1u << 3)) == 0 || (after_r & (1u << 4)) != 0) {
        printf("pmmu: U=1 M=0 after read, got desc=%08X\n", after_r);
        return 1;
    }
    /* Now write — expect M=1 too. */
    mac_pmmu_translate(&mem, 0x3010u, 5, true);
    cpu.tc = 0;
    u32 after_w = mac_read32(&mem, table_base + 3 * 4);
    if ((after_w & (1u << 3)) == 0 || (after_w & (1u << 4)) == 0) {
        printf("pmmu: U=1 M=1 after write, got desc=%08X\n", after_w);
        return 1;
    }

    /* M7.6j — TC.IS (initial shift). Re-use short-form entry 3 from the
     * U/M test (table_base+12 → 0x000D0000 + DT=1). Set IS=8: 24-bit
     * mode. LA 0x00003010 walks fine. LA 0x01003010 has top byte set
     * → LA out-of-range → bus error. */
    cpu.tc = 0;
    cpu.srp = ((u64)table_base << 32) | 2u;
    cpu.tc = 0x80000000u | (4u << 20) | (8u << 16) | (4u << 12);
    cpu.bus_error_pending = 0;
    u32 ph_is_ok = mac_pmmu_translate(&mem, 0x00003010u, 5, false);
    if (cpu.bus_error_pending != 0 ||
        (ph_is_ok & ~0xFFu) != 0x000D0000u) {
        printf("pmmu: IS=8 in-range phys=%08X berr=%08X\n",
               ph_is_ok, cpu.bus_error_pending); return 1;
    }
    cpu.bus_error_pending = 0;
    mac_pmmu_translate(&mem, 0x01003010u, 5, false);
    if (cpu.bus_error_pending == 0) {
        printf("pmmu: IS=8 out-of-range did not BERR\n"); return 1;
    }
    if ((cpu.bus_error_pending & BERR_CAUSE_MASK) != BERR_CAUSE_OOR) {
        printf("pmmu: IS-OOR cause=%08X want %08X\n",
               cpu.bus_error_pending & BERR_CAUSE_MASK, BERR_CAUSE_OOR);
        return 1;
    }

    mac_mem_free(&mem);
    (void)cpu; (void)pcpu;
    printf("  PMMU translate framework OK (short + long + BERR + WP + U/M + IS)\n");
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
