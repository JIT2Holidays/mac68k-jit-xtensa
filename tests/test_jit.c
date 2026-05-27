/* JIT differential test.
 *
 * Runs the built-in demo (and a couple of hand-built snippets) under both
 * the reference interpreter and the JIT, and asserts the final register
 * file, flags and cycle count match exactly. The JIT executes through the
 * in-tree Xtensa simulator on the host. */

#include "m68k_cpu.h"
#include "m68k_interp.h"
#include "mac_mem.h"
#include "demo_rom.h"
#include "dispatcher.h"
#include "m68k_asm.h"
#include <stdio.h>
#include <string.h>

static int diff_state(const char *name, const m68k_cpu *a, const m68k_cpu *b) {
    int bad = 0;
    for (int i = 0; i < 8; i++) {
        if (a->d[i] != b->d[i]) {
            printf("  %s: D%d interp=%08X jit=%08X\n", name, i, a->d[i], b->d[i]);
            bad = 1;
        }
        if (a->a[i] != b->a[i]) {
            printf("  %s: A%d interp=%08X jit=%08X\n", name, i, a->a[i], b->a[i]);
            bad = 1;
        }
    }
    if (a->pc != b->pc) {
        printf("  %s: PC interp=%06X jit=%06X\n", name, a->pc, b->pc); bad = 1;
    }
    if ((a->sr & 0x1F) != (b->sr & 0x1F)) {
        printf("  %s: CCR interp=%02X jit=%02X\n", name,
               a->sr & 0x1F, b->sr & 0x1F);
        bad = 1;
    }
    if (a->cycles != b->cycles) {
        printf("  %s: cycles interp=%llu jit=%llu\n", name,
               (unsigned long long)a->cycles, (unsigned long long)b->cycles);
        bad = 1;
    }
    if (a->exit_code != b->exit_code) {
        printf("  %s: exit interp=%d jit=%d\n", name, a->exit_code, b->exit_code);
        bad = 1;
    }
    return bad;
}

/* Run `img` under interp and JIT; compare. Returns 0 on match. */
static int run_both(const char *name, const u8 *img, u32 len, u64 budget) {
    mac_mem mi, mj;
    mac_mem_init(&mi, 1024 * 1024);
    mac_mem_init(&mj, 1024 * 1024);
    mac_load_ram_image(&mi, 0, img, len);
    mac_load_ram_image(&mj, 0, img, len);

    m68k_cpu ci, cj;
    m68k_reset(&ci, &mi);
    m68k_reset(&cj, &mj);

    m68k_run_until(&ci, budget);

    m68k_dispatcher d;
    if (!m68k_dispatcher_init(&d, &cj)) { printf("  %s: jit init failed\n", name); return 1; }
    m68k_dispatcher_run_until(&d, budget);

    int bad = diff_state(name, &ci, &cj);
    if (!bad) {
        printf("  %s: match (cycles=%llu blocks=%llu inline=%llu helper=%llu)\n",
               name, (unsigned long long)ci.cycles,
               (unsigned long long)d.blocks_compiled,
               (unsigned long long)d.inline_ops_total,
               (unsigned long long)d.helper_ops_total);
    }
    m68k_dispatcher_shutdown(&d);
    mac_mem_free(&mi);
    mac_mem_free(&mj);
    return bad;
}

/* snippet: MOVEQ-heavy (exercises the inline JIT path). */
static u32 build_moveq(u8 *img) {
    m68a a;
    m68a_init(&a, img, 512, 0);
    m68a_w32(&a, 0x00020000);
    m68a_w32(&a, 0x00000100);
    while (m68a_here(&a) < 0x100) m68a_w16(&a, 0x4E71);
    m68a_moveq(&a, 0, 7);
    m68a_moveq(&a, 1, -5);
    m68a_moveq(&a, 2, 0);
    m68a_nop(&a);
    m68a_moveq(&a, 3, 100);
    m68a_add_l(&a, 0, 1);
    m68a_add_l(&a, 0, 3);
    m68a_stop(&a, 0x2700);
    m68a_finish(&a);
    return a.len;
}

/* snippet: a Bcc.S loop. The m68a assembler emits only 16-bit-disp
 * branches, so short branches (the inlined JIT path) are hand-encoded.
 * D0 counts 5→0; BNE.S loops while D0 != 0. */
static u32 build_branch(u8 *img) {
    m68a a;
    m68a_init(&a, img, 512, 0);
    m68a_w32(&a, 0x00020000);              /* SSP */
    m68a_w32(&a, 0x00000100);              /* PC  */
    while (m68a_here(&a) < 0x100) m68a_w16(&a, 0x4E71);
    m68a_moveq(&a, 0, 5);                  /* D0 = 5            */
    m68a_w16(&a, 0x5380);                  /* loop: SUBQ.L #1,D0 */
    m68a_w16(&a, 0x66FC);                  /* BNE.S loop (-4)   */
    m68a_moveq(&a, 1, -3);                 /* D1 = -3           */
    m68a_w16(&a, 0x5281);                  /* L2: ADDQ.L #1,D1  */
    m68a_w16(&a, 0x6DFC);                  /* BLT.S L2 (D1<0)   */
    m68a_moveq(&a, 2, -3);                 /* D2 = -3           */
    m68a_w16(&a, 0x5282);                  /* L3: ADDQ.L #1,D2  */
    m68a_w16(&a, 0x6FFC);                  /* BLE.S L3 (D2<=0)  */
    m68a_w16(&a, 0x6002);                  /* BRA.S +2 (over)   */
    m68a_w16(&a, 0x4E71);                  /* NOP (skipped)     */
    m68a_stop(&a, 0x2700);
    m68a_finish(&a);
    return a.len;
}

int main(void) {
    int rc = 0;

    u8 snip[512];
    u32 slen = build_moveq(snip);
    rc |= run_both("moveq-snippet", snip, slen, 100000);

    u8 brsnip[512];
    u32 brlen = build_branch(brsnip);
    rc |= run_both("branch-loop", brsnip, brlen, 100000);

    /* OR.L / AND.L / SUB.L register forms (hand-encoded). */
    {
        u8 al[512];
        m68a a;
        m68a_init(&a, al, 512, 0);
        m68a_w32(&a, 0x00020000);
        m68a_w32(&a, 0x00000100);
        while (m68a_here(&a) < 0x100) m68a_w16(&a, 0x4E71);
        m68a_moveq(&a, 0, 0x55);
        m68a_moveq(&a, 1, 0x0F);
        m68a_w16(&a, 0x8081);              /* OR.L  D1,D0 */
        m68a_w16(&a, 0xC081);              /* AND.L D1,D0 */
        m68a_w16(&a, 0x9081);              /* SUB.L D1,D0 */
        m68a_stop(&a, 0x2700);
        m68a_finish(&a);
        rc |= run_both("alu-reg", al, a.len, 100000);
    }

    u8 demo[DEMO_ROM_MAX];
    mac_mem tmp;
    mac_mem_init(&tmp, 1024 * 1024);
    u32 dlen = demo_rom_build(demo, tmp.fb_base);
    mac_mem_free(&tmp);
    rc |= run_both("demo", demo, dlen, 50000000ull);

    /* M7.5a — EXTB.L JIT inline arm. Under SE/30 mode the block walker
     * keeps EXTB.L in the block (m68k_jit_can_inline_020 returns true)
     * and the JIT emits a 2-op slli/srai sequence. Validate against
     * the interpreter on three sign cases. */
    {
        mac_mem mi, mj;
        mac_mem_init_ex(&mi, MAC_MODEL_SE30, 64 * 1024);
        mac_mem_init_ex(&mj, MAC_MODEL_SE30, 64 * 1024);
        u8 prog[32];
        memset(prog, 0, sizeof prog);
        m68a a;
        m68a_init(&a, prog, sizeof prog, 0);
        m68a_w32(&a, 0x00010000);          /* SSP */
        m68a_w32(&a, 0x00000100);          /* PC  */
        m68a_finish(&a);
        mac_load_ram_image(&mi, 0, prog, 8);
        mac_load_ram_image(&mj, 0, prog, 8);
        /* MOVE.L #0x12345680,D0 ; EXTB.L D0 ; STOP. */
        u16 code[] = {
            0x203C, 0x1234, 0x5680,          /* MOVE.L #...,D0 */
            0x49C0,                          /* EXTB.L D0 */
            0x4E72, 0x2700,                  /* STOP #$2700 */
        };
        for (size_t i = 0; i < sizeof code / sizeof code[0]; i++) {
            mac_write16(&mi, 0x100u + (u32)(i * 2), code[i]);
            mac_write16(&mj, 0x100u + (u32)(i * 2), code[i]);
        }
        m68k_cpu ci, cj;
        m68k_reset(&ci, &mi);
        m68k_reset(&cj, &mj);
        m68k_run_until(&ci, 100000);
        m68k_dispatcher d;
        m68k_dispatcher_init(&d, &cj);
        m68k_dispatcher_run_until(&d, 100000);
        int bad = diff_state("se30-extbl", &ci, &cj);
        if (!bad) {
            if (ci.d[0] != 0xFFFFFF80) {
                printf("  se30-extbl: D0=%08X want FFFFFF80\n", ci.d[0]); bad = 1;
            } else {
                printf("  se30-extbl: match — D0=0xFFFFFF80 (sign-extended)\n");
            }
        }
        m68k_dispatcher_shutdown(&d);
        mac_mem_free(&mi);
        mac_mem_free(&mj);
        rc |= bad;
    }

    /* M7.5c — MOVEC lockstep. Block: MOVE.L #0x1000,D0 ; MOVEC D0,VBR ;
     * MOVEC VBR,D1 ; STOP. With MOVEC in can_inline_020 the JIT keeps
     * it in the block (via m68k_step bridge) instead of terminating.
     * Validate cpu->vbr and D1 match interp. */
    {
        mac_mem mi, mj;
        mac_mem_init_ex(&mi, MAC_MODEL_SE30, 64 * 1024);
        mac_mem_init_ex(&mj, MAC_MODEL_SE30, 64 * 1024);
        u8 prog[16];
        memset(prog, 0, sizeof prog);
        m68a aa;
        m68a_init(&aa, prog, sizeof prog, 0);
        m68a_w32(&aa, 0x00010000);
        m68a_w32(&aa, 0x00000100);
        m68a_finish(&aa);
        mac_load_ram_image(&mi, 0, prog, 8);
        mac_load_ram_image(&mj, 0, prog, 8);
        u16 code[] = {
            0x203C, 0x0000, 0x1000,            /* MOVE.L #0x1000,D0 */
            0x4E7B, (u16)((0 << 12) | 0x801),  /* MOVEC D0,VBR */
            0x4E7A, (u16)((1 << 12) | 0x801),  /* MOVEC VBR,D1 */
            0x4E72, 0x2700,                    /* STOP */
        };
        for (size_t i = 0; i < sizeof code / sizeof code[0]; i++) {
            mac_write16(&mi, 0x100u + (u32)(i * 2), code[i]);
            mac_write16(&mj, 0x100u + (u32)(i * 2), code[i]);
        }
        m68k_cpu ci, cj;
        m68k_reset(&ci, &mi);
        m68k_reset(&cj, &mj);
        m68k_run_until(&ci, 100000);
        m68k_dispatcher d;
        m68k_dispatcher_init(&d, &cj);
        m68k_dispatcher_run_until(&d, 100000);
        int bad = diff_state("se30-movec", &ci, &cj);
        if (!bad) {
            if (ci.vbr != 0x1000 || ci.d[1] != 0x1000 || cj.vbr != 0x1000 || cj.d[1] != 0x1000) {
                printf("  se30-movec: VBR=%08X D1=%08X (jit VBR=%08X D1=%08X) want 1000/1000\n",
                       ci.vbr, ci.d[1], cj.vbr, cj.d[1]); bad = 1;
            } else {
                printf("  se30-movec: match — VBR=0x1000, D1=0x1000\n");
            }
        }
        m68k_dispatcher_shutdown(&d);
        mac_mem_free(&mi);
        mac_mem_free(&mj);
        rc |= bad;
    }

    /* M7.5b — LINK.L decode test. Validates m68k_decode_at sizes the
     * 6-byte LINK.L correctly so the block walker doesn't fall into the
     * d32 displacement bytes mis-decoded as instructions. */
    {
        mac_mem mi, mj;
        mac_mem_init_ex(&mi, MAC_MODEL_SE30, 64 * 1024);
        mac_mem_init_ex(&mj, MAC_MODEL_SE30, 64 * 1024);
        u8 prog[64];
        memset(prog, 0, sizeof prog);
        m68a aa;
        m68a_init(&aa, prog, sizeof prog, 0);
        m68a_w32(&aa, 0x00010000);
        m68a_w32(&aa, 0x00000100);
        m68a_finish(&aa);
        mac_load_ram_image(&mi, 0, prog, 8);
        mac_load_ram_image(&mj, 0, prog, 8);
        /* MOVEA.L #0x4000,A6 ; MOVEA.L #0x4000,A7 ; LINK.L A6,#-0x10000 ; STOP. */
        u16 code[] = {
            0x2C7C, 0x0000, 0x4000,           /* MOVEA.L #0x00004000, A6 */
            0x2E7C, 0x0000, 0x4000,           /* MOVEA.L #0x00004000, A7 */
            0x480E, 0xFFFF, 0x0000,           /* LINK.L A6, #-0x10000 */
            0x4E72, 0x2700,                   /* STOP #$2700 */
        };
        for (size_t i = 0; i < sizeof code / sizeof code[0]; i++) {
            mac_write16(&mi, 0x100u + (u32)(i * 2), code[i]);
            mac_write16(&mj, 0x100u + (u32)(i * 2), code[i]);
        }
        m68k_cpu ci, cj;
        m68k_reset(&ci, &mi);
        m68k_reset(&cj, &mj);
        m68k_run_until(&ci, 100000);
        m68k_dispatcher d;
        m68k_dispatcher_init(&d, &cj);
        m68k_dispatcher_run_until(&d, 100000);
        int bad = diff_state("se30-linkl", &ci, &cj);
        if (!bad) printf("  se30-linkl: match (LINK.L correctly sized in walker)\n");
        m68k_dispatcher_shutdown(&d);
        mac_mem_free(&mi);
        mac_mem_free(&mj);
        rc |= bad;
    }

    /* M7.5b — RTD decode test. Validates m68k_decode_at marks RTD as
     * a block terminator so the walker stops before reading bytes past
     * RTD as phantom opcodes. */
    {
        mac_mem mi, mj;
        mac_mem_init_ex(&mi, MAC_MODEL_SE30, 64 * 1024);
        mac_mem_init_ex(&mj, MAC_MODEL_SE30, 64 * 1024);
        u8 prog[64];
        memset(prog, 0, sizeof prog);
        m68a aa;
        m68a_init(&aa, prog, sizeof prog, 0);
        m68a_w32(&aa, 0x00010000);
        m68a_w32(&aa, 0x00000100);
        m68a_finish(&aa);
        mac_load_ram_image(&mi, 0, prog, 8);
        mac_load_ram_image(&mj, 0, prog, 8);
        /* Put a return address 0x200 at SP=0x1000, set up SP, then RTD #8
         * which should pop PC=0x200, SP+=4+8 = 0x100C. Then STOP at 0x200. */
        mac_write32(&mi, 0x1000, 0x00000200);
        mac_write32(&mj, 0x1000, 0x00000200);
        mac_write16(&mi, 0x200, 0x4E72);     /* STOP */
        mac_write16(&mi, 0x202, 0x2700);
        mac_write16(&mj, 0x200, 0x4E72);
        mac_write16(&mj, 0x202, 0x2700);
        /* MOVEA.L #0x1000, A7 ; RTD #8. */
        u16 code[] = {
            0x2E7C, 0x0000, 0x1000,           /* MOVEA.L #0x1000, A7 */
            0x4E74, 0x0008,                   /* RTD #8 */
        };
        for (size_t i = 0; i < sizeof code / sizeof code[0]; i++) {
            mac_write16(&mi, 0x100u + (u32)(i * 2), code[i]);
            mac_write16(&mj, 0x100u + (u32)(i * 2), code[i]);
        }
        m68k_cpu ci, cj;
        m68k_reset(&ci, &mi);
        m68k_reset(&cj, &mj);
        m68k_run_until(&ci, 100000);
        m68k_dispatcher d;
        m68k_dispatcher_init(&d, &cj);
        m68k_dispatcher_run_until(&d, 100000);
        int bad = diff_state("se30-rtd", &ci, &cj);
        if (!bad) {
            if (ci.pc != 0x204 || ci.a[7] != 0x100C) {
                printf("  se30-rtd: PC=%08X SP=%08X want 0x204/0x100C\n",
                       ci.pc, ci.a[7]); bad = 1;
            } else {
                printf("  se30-rtd: match — PC=0x204, SP=0x100C\n");
            }
        }
        m68k_dispatcher_shutdown(&d);
        mac_mem_free(&mi);
        mac_mem_free(&mj);
        rc |= bad;
    }

    /* M7.5p — BFCLR / BFSET / BFCHG Dn{off:wid} native inline arms.
     * Chain: clear bits 4-11 of D0, then set bits 16-23, then complement
     * bits 24-31. */
    {
        mac_mem mi, mj;
        mac_mem_init_ex(&mi, MAC_MODEL_SE30, 64 * 1024);
        mac_mem_init_ex(&mj, MAC_MODEL_SE30, 64 * 1024);
        u8 prog[16];
        memset(prog, 0, sizeof prog);
        m68a aa;
        m68a_init(&aa, prog, sizeof prog, 0);
        m68a_w32(&aa, 0x00010000);
        m68a_w32(&aa, 0x00000100);
        m68a_finish(&aa);
        mac_load_ram_image(&mi, 0, prog, 8);
        mac_load_ram_image(&mj, 0, prog, 8);
        u16 code[] = {
            0x203C, 0xFFFF, 0x00FF,          /* MOVE.L #0xFFFF00FF, D0 */
            0xECC0, (u16)((4 << 6) | 8),     /* BFCLR D0{4:8} — clear bits 4..11 */
            0xEEC0, (u16)((16 << 6) | 8),    /* BFSET D0{16:8} — set bits 16..23 */
            0xEAC0, (u16)((0 << 6) | 8),     /* BFCHG D0{0:8} — toggle bits 0..7 */
            0x4E72, 0x2700,                  /* STOP */
        };
        for (size_t i = 0; i < sizeof code / sizeof code[0]; i++) {
            mac_write16(&mi, 0x100u + (u32)(i * 2), code[i]);
            mac_write16(&mj, 0x100u + (u32)(i * 2), code[i]);
        }
        m68k_cpu ci, cj;
        m68k_reset(&ci, &mi);
        m68k_reset(&cj, &mj);
        m68k_run_until(&ci, 100000);
        m68k_dispatcher d;
        m68k_dispatcher_init(&d, &cj);
        m68k_dispatcher_run_until(&d, 100000);
        int bad = diff_state("se30-bfclr-set-chg", &ci, &cj);
        /* Initial D0 = 0xFFFF00FF.
         * BFCLR D0{4:8} clears bits 4..11 of D0 (off=4 → bit 27..20 from
         * MSB indexing = bits 27..20 of register). Wait — off is from MSB.
         * off=4, wid=8 → bits (31-4)..(31-4-8+1) = 27..20 of register.
         * So mask = 0x0FF00000. After BFCLR: D0 = 0xFFFF00FF & ~0x0FF00000 = 0xF00F00FF.
         * BFSET D0{16:8}: off=16, wid=8 → bits 15..8. mask = 0x0000FF00.
         * D0 = 0xF00F00FF | 0x0000FF00 = 0xF00FFFFF.
         * BFCHG D0{0:8}: off=0, wid=8 → bits 31..24. mask = 0xFF000000.
         * D0 = 0xF00FFFFF ^ 0xFF000000 = 0x0F0FFFFF. */
        if (!bad) {
            if (ci.d[0] != 0x0F0FFFFFu) {
                printf("  se30-bfclr-set-chg: D0=%08X want 0F0FFFFF\n", ci.d[0]); bad = 1;
            } else {
                printf("  se30-bfclr-set-chg: match — D0=0x0F0FFFFF\n");
            }
        }
        m68k_dispatcher_shutdown(&d);
        mac_mem_free(&mi);
        mac_mem_free(&mj);
        rc |= bad;
    }

    /* M7.5n — BFTST Dn{off:wid} native inline arm (flags-only). */
    {
        mac_mem mi, mj;
        mac_mem_init_ex(&mi, MAC_MODEL_SE30, 64 * 1024);
        mac_mem_init_ex(&mj, MAC_MODEL_SE30, 64 * 1024);
        u8 prog[16];
        memset(prog, 0, sizeof prog);
        m68a aa;
        m68a_init(&aa, prog, sizeof prog, 0);
        m68a_w32(&aa, 0x00010000);
        m68a_w32(&aa, 0x00000100);
        m68a_finish(&aa);
        mac_load_ram_image(&mi, 0, prog, 8);
        mac_load_ram_image(&mj, 0, prog, 8);
        u16 code[] = {
            0x203C, 0x00FF, 0xFF00,           /* MOVE.L #0x00FFFF00,D0 — field "FF" at offset 8 */
            0xE8C0, (u16)((8 << 6) | 8),      /* BFTST D0{8:8}  — field = 0xFF, N=1, Z=0 */
            0x40C2,                           /* MOVE SR, D2 (capture CCR before STOP) */
            0x4E72, 0x2700,                   /* STOP */
        };
        for (size_t i = 0; i < sizeof code / sizeof code[0]; i++) {
            mac_write16(&mi, 0x100u + (u32)(i * 2), code[i]);
            mac_write16(&mj, 0x100u + (u32)(i * 2), code[i]);
        }
        m68k_cpu ci, cj;
        m68k_reset(&ci, &mi);
        m68k_reset(&cj, &mj);
        m68k_run_until(&ci, 100000);
        m68k_dispatcher d;
        m68k_dispatcher_init(&d, &cj);
        m68k_dispatcher_run_until(&d, 100000);
        int bad = diff_state("se30-bftst-dn", &ci, &cj);
        if (!bad) {
            /* D2 captured SR after BFTST. Low 5 bits = CCR. Expect
             * N=1 (bit 3), Z=0 (field 0xFF: MSB set, nonzero). */
            u8 ccr = (u8)(ci.d[2] & 0x1F);
            if (!(ccr & CCR_N) || (ccr & CCR_Z)) {
                printf("  se30-bftst-dn: CCR=%02X want N=1 Z=0\n", ccr); bad = 1;
            } else {
                printf("  se30-bftst-dn: match — CCR=%02X (N=1, Z=0)\n", ccr);
            }
        }
        m68k_dispatcher_shutdown(&d);
        mac_mem_free(&mi);
        mac_mem_free(&mj);
        rc |= bad;
    }

    /* M7.5o — BFEXTS Dn{off:wid},Dm native inline arm (signed extract). */
    {
        mac_mem mi, mj;
        mac_mem_init_ex(&mi, MAC_MODEL_SE30, 64 * 1024);
        mac_mem_init_ex(&mj, MAC_MODEL_SE30, 64 * 1024);
        u8 prog[16];
        memset(prog, 0, sizeof prog);
        m68a aa;
        m68a_init(&aa, prog, sizeof prog, 0);
        m68a_w32(&aa, 0x00010000);
        m68a_w32(&aa, 0x00000100);
        m68a_finish(&aa);
        mac_load_ram_image(&mi, 0, prog, 8);
        mac_load_ram_image(&mj, 0, prog, 8);
        /* MOVE.L #0xABCD1234, D0 ; BFEXTS D0{4:8}, D1.
         * Field bits 4..11 of D0 = 0xBC. Sign-extended (top bit 1) →
         * D1 = 0xFFFFFFBC. */
        u16 code[] = {
            0x203C, 0xABCD, 0x1234,
            0xEBC0, (u16)((1 << 12) | (4 << 6) | 8), /* BFEXTS D0{4:8}, D1 */
            0x4E72, 0x2700,
        };
        for (size_t i = 0; i < sizeof code / sizeof code[0]; i++) {
            mac_write16(&mi, 0x100u + (u32)(i * 2), code[i]);
            mac_write16(&mj, 0x100u + (u32)(i * 2), code[i]);
        }
        m68k_cpu ci, cj;
        m68k_reset(&ci, &mi);
        m68k_reset(&cj, &mj);
        m68k_run_until(&ci, 100000);
        m68k_dispatcher d;
        m68k_dispatcher_init(&d, &cj);
        m68k_dispatcher_run_until(&d, 100000);
        int bad = diff_state("se30-bfexts-dn", &ci, &cj);
        if (!bad) {
            if (ci.d[1] != 0xFFFFFFBC) {
                printf("  se30-bfexts-dn: D1=%08X want FFFFFFBC\n", ci.d[1]); bad = 1;
            } else {
                printf("  se30-bfexts-dn: match — D1=0xFFFFFFBC (sign-extended)\n");
            }
        }
        m68k_dispatcher_shutdown(&d);
        mac_mem_free(&mi);
        mac_mem_free(&mj);
        rc |= bad;
    }

    /* M7.5m — BFEXTU Dn{off:wid},Dm native inline arm lockstep.
     * Specifically exercises the new register-source/static-off/wid
     * codegen path: MOVE.L #0xABCD1234, D0 ; BFEXTU D0{4:8}, D1 ; STOP.
     * Expected: D1 = 0xBC, N=1, Z=0. */
    {
        mac_mem mi, mj;
        mac_mem_init_ex(&mi, MAC_MODEL_SE30, 64 * 1024);
        mac_mem_init_ex(&mj, MAC_MODEL_SE30, 64 * 1024);
        u8 prog[16];
        memset(prog, 0, sizeof prog);
        m68a aa;
        m68a_init(&aa, prog, sizeof prog, 0);
        m68a_w32(&aa, 0x00010000);
        m68a_w32(&aa, 0x00000100);
        m68a_finish(&aa);
        mac_load_ram_image(&mi, 0, prog, 8);
        mac_load_ram_image(&mj, 0, prog, 8);
        u16 code[] = {
            0x203C, 0xABCD, 0x1234,           /* MOVE.L #0xABCD1234, D0 */
            0xE9C0, (u16)((1 << 12) | (4 << 6) | 8), /* BFEXTU D0{4:8}, D1 */
            0x4E72, 0x2700,                   /* STOP */
        };
        for (size_t i = 0; i < sizeof code / sizeof code[0]; i++) {
            mac_write16(&mi, 0x100u + (u32)(i * 2), code[i]);
            mac_write16(&mj, 0x100u + (u32)(i * 2), code[i]);
        }
        m68k_cpu ci, cj;
        m68k_reset(&ci, &mi);
        m68k_reset(&cj, &mj);
        m68k_run_until(&ci, 100000);
        m68k_dispatcher d;
        m68k_dispatcher_init(&d, &cj);
        m68k_dispatcher_run_until(&d, 100000);
        int bad = diff_state("se30-bfextu-dn", &ci, &cj);
        if (!bad) {
            if (ci.d[1] != 0xBC) {
                printf("  se30-bfextu-dn: D1=%08X want BC\n", ci.d[1]); bad = 1;
            } else {
                printf("  se30-bfextu-dn: match — D1=0xBC (Dn-source BFEXTU)\n");
            }
        }
        m68k_dispatcher_shutdown(&d);
        mac_mem_free(&mi);
        mac_mem_free(&mj);
        rc |= bad;
    }

    /* M7.5h — CINV/CPUSH native inline arm lockstep. CINV BC,(A0) +
     * CPUSH BC,(A1) — both no-ops in our model. */
    {
        mac_mem mi, mj;
        mac_mem_init_ex(&mi, MAC_MODEL_SE30, 64 * 1024);
        mac_mem_init_ex(&mj, MAC_MODEL_SE30, 64 * 1024);
        u8 prog[16];
        memset(prog, 0, sizeof prog);
        m68a aa;
        m68a_init(&aa, prog, sizeof prog, 0);
        m68a_w32(&aa, 0x00010000);
        m68a_w32(&aa, 0x00000100);
        m68a_finish(&aa);
        mac_load_ram_image(&mi, 0, prog, 8);
        mac_load_ram_image(&mj, 0, prog, 8);
        /* CINV BC,(A0): 1111 0100 0011 1000 = 0xF438.
         * CPUSH BC,(A1): 1111 0100 0111 1001 = 0xF479.
         * Just to make sure both engines advance PC + cycles equally. */
        u16 code[] = {
            0x7064,                            /* MOVEQ #100, D0 */
            0xF438,                            /* CINV BC,(A0) */
            0xF479,                            /* CPUSH BC,(A1) */
            0x4E72, 0x2700,                    /* STOP */
        };
        for (size_t i = 0; i < sizeof code / sizeof code[0]; i++) {
            mac_write16(&mi, 0x100u + (u32)(i * 2), code[i]);
            mac_write16(&mj, 0x100u + (u32)(i * 2), code[i]);
        }
        m68k_cpu ci, cj;
        m68k_reset(&ci, &mi);
        m68k_reset(&cj, &mj);
        m68k_run_until(&ci, 100000);
        m68k_dispatcher d;
        m68k_dispatcher_init(&d, &cj);
        m68k_dispatcher_run_until(&d, 100000);
        int bad = diff_state("se30-cinv-cpush", &ci, &cj);
        if (!bad) printf("  se30-cinv-cpush: match — D0=100, both no-op cycles agree\n");
        m68k_dispatcher_shutdown(&d);
        mac_mem_free(&mi);
        mac_mem_free(&mj);
        rc |= bad;
    }

    /* M7.5f — PACK / UNPK + TRAPcc lockstep. Block:
     *   MOVEQ #$5, D0           ; D0 = 0x05
     *   MOVEQ #$7, D1           ; D1 = 0x07  (but PACK uses D0 src)
     *   MOVE.W #$3735, D0       ; D0 low word = '7' << 8 | '5'
     *   PACK D0, D1, #0         ; D1 = 0x57
     *   UNPK D1, D2, #$3030     ; D2 = 0x3037 (or similar)
     *   TRAPF                   ; never traps (cc=False)
     *   STOP. */
    {
        mac_mem mi, mj;
        mac_mem_init_ex(&mi, MAC_MODEL_SE30, 64 * 1024);
        mac_mem_init_ex(&mj, MAC_MODEL_SE30, 64 * 1024);
        u8 prog[16];
        memset(prog, 0, sizeof prog);
        m68a aa;
        m68a_init(&aa, prog, sizeof prog, 0);
        m68a_w32(&aa, 0x00010000);
        m68a_w32(&aa, 0x00000100);
        m68a_finish(&aa);
        mac_load_ram_image(&mi, 0, prog, 8);
        mac_load_ram_image(&mj, 0, prog, 8);
        u16 code[] = {
            0x303C, 0x3735,                  /* MOVE.W #$3735, D0 (PACK src) */
            0x8340, 0x0000,                  /* PACK D0,D1,#$0  — bits 11-9 = 001 (D1 dst) */
            0xC580, 0x3030,                  /* UNPK D0,D2,#$3030 — D2 from D0 */
            0x51FC,                          /* TRAPF (cc=F never traps) */
            0x4E72, 0x2700,                  /* STOP */
        };
        for (size_t i = 0; i < sizeof code / sizeof code[0]; i++) {
            mac_write16(&mi, 0x100u + (u32)(i * 2), code[i]);
            mac_write16(&mj, 0x100u + (u32)(i * 2), code[i]);
        }
        m68k_cpu ci, cj;
        m68k_reset(&ci, &mi);
        m68k_reset(&cj, &mj);
        m68k_run_until(&ci, 100000);
        m68k_dispatcher d;
        m68k_dispatcher_init(&d, &cj);
        m68k_dispatcher_run_until(&d, 100000);
        int bad = diff_state("se30-pack-trapf", &ci, &cj);
        if (!bad) printf("  se30-pack-trapf: match — D1=%02X D2=%04X\n",
                         ci.d[1] & 0xFF, ci.d[2] & 0xFFFF);
        m68k_dispatcher_shutdown(&d);
        mac_mem_free(&mi);
        mac_mem_free(&mj);
        rc |= bad;
    }

    /* M7.5e — MULU.L / DIVU.L lockstep. MOVE.L #20, D0 ; MULU.L #6, D0 ;
     * DIVU.L #4, D0 ; STOP. With sizing fixed and these ops in
     * can_inline_020, the block contains all of them through the
     * m68k_step bridge. */
    {
        mac_mem mi, mj;
        mac_mem_init_ex(&mi, MAC_MODEL_SE30, 64 * 1024);
        mac_mem_init_ex(&mj, MAC_MODEL_SE30, 64 * 1024);
        u8 prog[16];
        memset(prog, 0, sizeof prog);
        m68a aa;
        m68a_init(&aa, prog, sizeof prog, 0);
        m68a_w32(&aa, 0x00010000);
        m68a_w32(&aa, 0x00000100);
        m68a_finish(&aa);
        mac_load_ram_image(&mi, 0, prog, 8);
        mac_load_ram_image(&mj, 0, prog, 8);
        u16 code[] = {
            0x203C, 0x0000, 0x0014,            /* MOVE.L #20, D0 */
            0x4C3C, 0x0800, 0x0000, 0x0006,    /* MULU.L #6, D0 */
                                               /* ext bits 14-12=0(Dq), 11=1(sgnoff in our impl=signed bit, hmm) */
            0x4C7C, 0x0800, 0x0000, 0x0004,    /* DIVU.L #4, D0 */
            0x4E72, 0x2700,                    /* STOP */
        };
        /* MUL.L ext word: bits 14-12 = Dq, bit 11 = signed (1=signed, 0=unsigned),
         * bit 10 = 64-bit result. For 32-bit unsigned product into D0 use
         * Dq=0, unsigned (bit 11 = 0), 32-bit (bit 10 = 0): ext = 0x0000.
         * Let's flip to actually unsigned. */
        code[4] = 0x0000;          /* MULU.L: Dq=0, unsigned, 32-bit */
        code[8] = 0x0000;          /* DIVU.L: Dq=0, unsigned, 32-bit */
        for (size_t i = 0; i < sizeof code / sizeof code[0]; i++) {
            mac_write16(&mi, 0x100u + (u32)(i * 2), code[i]);
            mac_write16(&mj, 0x100u + (u32)(i * 2), code[i]);
        }
        m68k_cpu ci, cj;
        m68k_reset(&ci, &mi);
        m68k_reset(&cj, &mj);
        m68k_run_until(&ci, 100000);
        m68k_dispatcher d;
        m68k_dispatcher_init(&d, &cj);
        m68k_dispatcher_run_until(&d, 100000);
        int bad = diff_state("se30-muldiv-l", &ci, &cj);
        if (!bad) {
            /* 20 * 6 = 120; 120 / 4 = 30. */
            if (ci.d[0] != 30) {
                printf("  se30-muldiv-l: D0=%u want 30\n", ci.d[0]); bad = 1;
            } else {
                printf("  se30-muldiv-l: match — D0=30 (20*6/4)\n");
            }
        }
        m68k_dispatcher_shutdown(&d);
        mac_mem_free(&mi);
        mac_mem_free(&mj);
        rc |= bad;
    }

    /* M7.5d — bitfield in memory mode (BFEXTU (d16,An){off:wid},Dn) —
     * validates the decoder fix that adds the d16 displacement bytes
     * to d.length. Without it, the JIT walker reads the d16 word as
     * a phantom opcode. */
    {
        mac_mem mi, mj;
        mac_mem_init_ex(&mi, MAC_MODEL_SE30, 64 * 1024);
        mac_mem_init_ex(&mj, MAC_MODEL_SE30, 64 * 1024);
        u8 prog[64];
        memset(prog, 0, sizeof prog);
        m68a aa;
        m68a_init(&aa, prog, sizeof prog, 0);
        m68a_w32(&aa, 0x00010000);
        m68a_w32(&aa, 0x00000100);
        m68a_finish(&aa);
        mac_load_ram_image(&mi, 0, prog, 8);
        mac_load_ram_image(&mj, 0, prog, 8);
        /* Put 0xABCD1234 at address 0x1000. Code:
         *   MOVEA.L #0x1000, A0
         *   BFEXTU (8,A0){0:8}, D1     ; D1 = byte at A0+8 -> garbage
         *   STOP.
         * Simpler: BFEXTU (A0){4:8}, D1 — mode 2 (no d16). Actually the
         * "memory" test we want is mode 5 with d16, which forces the
         * decoder to size as 6 bytes. */
        mac_write32(&mi, 0x1008, 0xABCD1234u);
        mac_write32(&mj, 0x1008, 0xABCD1234u);
        u16 code[] = {
            0x207C, 0x0000, 0x1000,                  /* MOVEA.L #0x1000, A0 */
            0xE9E8,                                  /* BFEXTU (d16,A0){...},D1; ext at next word */
            (u16)((1 << 12) | (4 << 6) | 8),         /* ext: dst=D1, off=4, wid=8 */
            0x0008,                                  /* d16 = 8 */
            0x4E72, 0x2700,                          /* STOP */
        };
        for (size_t i = 0; i < sizeof code / sizeof code[0]; i++) {
            mac_write16(&mi, 0x100u + (u32)(i * 2), code[i]);
            mac_write16(&mj, 0x100u + (u32)(i * 2), code[i]);
        }
        m68k_cpu ci, cj;
        m68k_reset(&ci, &mi);
        m68k_reset(&cj, &mj);
        m68k_run_until(&ci, 100000);
        m68k_dispatcher d;
        m68k_dispatcher_init(&d, &cj);
        m68k_dispatcher_run_until(&d, 100000);
        int bad = diff_state("se30-bfextu-mem", &ci, &cj);
        if (!bad) {
            /* (0xABCD1234 starting at bit-offset 4 from MSB, width 8) = 0xBC. */
            if (ci.d[1] != 0xBC) {
                printf("  se30-bfextu-mem: D1=%08X want 000000BC\n", ci.d[1]); bad = 1;
            } else {
                printf("  se30-bfextu-mem: match — D1=0xBC (mem BFEXTU)\n");
            }
        }
        m68k_dispatcher_shutdown(&d);
        mac_mem_free(&mi);
        mac_mem_free(&mj);
        rc |= bad;
    }

    /* SE/30 hybrid termination: a block that mixes 68000 ops with a
     * 68020 BFEXTU. The JIT block walker should stop just before BFEXTU
     * and the interpreter should pick up there. End state must match
     * pure-interp execution on SE/30 mode. */
    {
        mac_mem mi, mj;
        mac_mem_init_ex(&mi, MAC_MODEL_SE30, 256 * 1024);
        mac_mem_init_ex(&mj, MAC_MODEL_SE30, 256 * 1024);
        /* Initial SP / PC vectors in low RAM. */
        u8 prog[64];
        memset(prog, 0, sizeof prog);
        m68a a;
        m68a_init(&a, prog, sizeof prog, 0);
        m68a_w32(&a, 0x00020000);              /* SSP */
        m68a_w32(&a, 0x00000100);              /* PC  */
        m68a_finish(&a);
        mac_load_ram_image(&mi, 0, prog, 8);
        mac_load_ram_image(&mj, 0, prog, 8);
        /* Code at 0x100: MOVE.L #0xABCD1234, D0 ; BFEXTU D0{4:8}, D1 ; STOP */
        u16 code[] = {
            0x203C, 0xABCD, 0x1234,             /* MOVE.L #...,D0 */
            0xE9C0, (u16)((1 << 12) | (4 << 6) | 8),  /* BFEXTU D0{4:8}, D1 */
            0x4E72, 0x2700,                     /* STOP #$2700 */
        };
        for (size_t i = 0; i < sizeof code / sizeof code[0]; i++) {
            mac_write16(&mi, 0x100u + (u32)(i * 2), code[i]);
            mac_write16(&mj, 0x100u + (u32)(i * 2), code[i]);
        }

        m68k_cpu ci, cj;
        m68k_reset(&ci, &mi);
        m68k_reset(&cj, &mj);
        m68k_run_until(&ci, 100000);

        m68k_dispatcher d;
        m68k_dispatcher_init(&d, &cj);
        m68k_dispatcher_run_until(&d, 100000);
        int bad = diff_state("se30-hybrid", &ci, &cj);
        if (!bad) {
            if (ci.d[1] != 0xBC) {
                printf("  se30-hybrid: D1=%08X want 000000BC\n", ci.d[1]);
                bad = 1;
            } else {
                printf("  se30-hybrid: match (interp+JIT both produced D1=0xBC)\n");
            }
        }
        m68k_dispatcher_shutdown(&d);
        mac_mem_free(&mi);
        mac_mem_free(&mj);
        rc |= bad;
    }

    if (rc) { printf("FAIL: JIT differential\n"); return 1; }
    printf("PASS: JIT differential\n");
    return 0;
}
