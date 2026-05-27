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
