/* Xtensa encoder + simulator round-trip test.
 *
 * Emits a short Xtensa program with the JIT's encoder and executes it on
 * the in-tree simulator — a shared bug would have to corrupt both the
 * encoder and the independently-written decoder identically to slip
 * through. */

#include "emit_xtensa.h"
#include "xtensa_sim.h"
#include <stdio.h>
#include <string.h>

static u8 g_mem[256];

static u8 *translate(xt_sim *s, u32 addr) {
    (void)s;
    if (addr < sizeof(g_mem)) return g_mem + addr;
    return NULL;
}

int main(void) {
    u8 code[256];
    xt_emit e;
    xt_init(&e, code, sizeof(code));

    /* a2 = 5; a3 = 37; a4 = a2 + a3; a5 = a4 & 0x0F; a6 = a5 << 2;
     * store a6 to g_mem[0] as a 32-bit word; RET. */
    xt_movi(&e, 2, 5);
    xt_movi(&e, 3, 37);
    xt_add (&e, 4, 2, 3);          /* 42 */
    xt_movi(&e, 7, 0x0F);
    xt_and (&e, 5, 4, 7);          /* 42 & 0x0F = 0x0A */
    xt_slli(&e, 6, 5, 2);          /* 0x0A << 2 = 0x28 */
    xt_movi(&e, 8, 0);
    xt_s32i(&e, 6, 8, 0);          /* g_mem[0..3] = a6 */
    xt_ret (&e);
    xt_flush_pending(&e);

    if (e.overflow) { printf("FAIL: encoder overflow\n"); return 1; }

    memset(g_mem, 0, sizeof(g_mem));
    xt_sim s;
    xt_sim_init(&s, code, e.len);
    s.translate = translate;
    s.a[0] = 0;                    /* RET sentinel */
    xt_sim_run(&s, 1000);

    if (s.status != XT_SIM_RETURNED) {
        printf("FAIL: sim status=%d\n", (int)s.status);
        return 1;
    }
    if (s.a[4] != 42)   { printf("FAIL: a4=%u want 42\n", s.a[4]); return 1; }
    if (s.a[6] != 0x28) { printf("FAIL: a6=%u want 0x28\n", s.a[6]); return 1; }
    u32 stored = (u32)g_mem[0] | ((u32)g_mem[1] << 8) |
                 ((u32)g_mem[2] << 16) | ((u32)g_mem[3] << 24);
    if (stored != 0x28) { printf("FAIL: stored=%u want 0x28\n", stored); return 1; }

    /* Branch check: count down a register with BNEZ. */
    xt_init(&e, code, sizeof(code));
    xt_movi(&e, 2, 5);             /* counter */
    xt_movi(&e, 3, 0);             /* accumulator */
    u32 loop = e.len;
    xt_addi(&e, 3, 3, 1);
    xt_addi(&e, 2, 2, -1);
    xt_bnez(&e, 2, (i32)loop - (i32)e.len);
    xt_ret(&e);
    xt_flush_pending(&e);

    xt_sim_init(&s, code, e.len);
    s.a[0] = 0;
    xt_sim_run(&s, 1000);
    if (s.status != XT_SIM_RETURNED || s.a[3] != 5) {
        printf("FAIL: loop a3=%u want 5 status=%d\n", s.a[3], (int)s.status);
        return 1;
    }

    printf("PASS: encoder + simulator\n");
    return 0;
}
