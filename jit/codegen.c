/* 68000 -> Xtensa LX7 basic-block JIT.
 *
 * Strategy (see codegen.h): a curated fast set is emitted inline; every
 * other instruction becomes a CALLX0 to the reference interpreter's
 * m68k_step. The interpreter fallback makes the JIT correct by
 * construction — the JIT differential test exists to catch the inline
 * paths drifting from the oracle. */

#include "codegen.h"
#include "emit_xtensa.h"
#include "m68k_interp.h"
#include "mac_mem.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>

/* cpu_state field byte offsets — resolved by the compiler, never guessed. */
#define OFF_D(n)     ((u32)(offsetof(m68k_cpu, d) + (n) * 4u))
#define OFF_A(n)     ((u32)(offsetof(m68k_cpu, a) + (n) * 4u))
#define OFF_PC       ((u32)offsetof(m68k_cpu, pc))
#define OFF_SR       ((u32)offsetof(m68k_cpu, sr))
#define OFF_CYCLES   ((u32)offsetof(m68k_cpu, cycles))   /* low 32 bits */
#define OFF_JITRETPC ((u32)offsetof(m68k_cpu, jit_ret_pc))

/* Xtensa registers reserved across a block. */
#define R_ARG   2     /* CALLX0 first argument          */
#define R_CPU   3     /* cpu_state base (survives calls) */
#define R_HELP 14     /* CALLX0 target scratch          */
/* a8..a13, a15 are inline-op scratch. */

/* Worst-case bytes any single instruction's emission can need. The fattest
 * inline body (ADD.L with full flag computation) is ~40 narrow-form ops at
 * 3 bytes each. */
#define BYTES_PER_OP        160u
#define PROLOGUE_EPILOGUE   48u

static u32 align_up_4(u32 v) { return (v + 3u) & ~3u; }

/* Encode an L32R loading the literal at byte offset `lit_off` into `at`,
 * where the L32R instruction itself sits at block offset `pc_off`. */
static void emit_l32r_at(xt_emit *e, u8 at, u32 lit_off, u32 pc_off) {
    u32 pc_aligned = (pc_off + 3u) & ~3u;
    assert(lit_off < pc_aligned);
    u32 dist = pc_aligned - lit_off;
    assert((dist & 3u) == 0u);
    u32 imm16 = 0x10000u - (dist >> 2);   /* negative 16-bit field */
    xt_l32r(e, at, imm16);
}

/* --- inline op emitters ----------------------------------------------- */

/* cpu->pc += delta ; cpu->cycles += cyc  (low 32 bits, carry ignored). */
static void emit_advance(xt_emit *e, i32 pc_delta, i32 cyc) {
    xt_l32i(e, 8, R_CPU, OFF_PC);
    xt_addi(e, 8, 8, pc_delta);
    xt_s32i(e, 8, R_CPU, OFF_PC);
    xt_l32i(e, 8, R_CPU, OFF_CYCLES);
    xt_addi(e, 8, 8, cyc);
    xt_s32i(e, 8, R_CPU, OFF_CYCLES);
}

/* Build an arbitrary 32-bit constant into `dst` (movi only reaches 12
 * bits). `tmp` is a scratch register distinct from `dst`. */
static void emit_load_imm32(xt_emit *e, u8 dst, u8 tmp, u32 val) {
    xt_movi(e, dst, (i32)((val >> 24) & 0xFF));
    int sh[3] = { 16, 8, 0 };
    for (int i = 0; i < 3; i++) {
        xt_slli(e, dst, dst, 8);
        xt_movi(e, tmp, (i32)((val >> sh[i]) & 0xFF));
        xt_or  (e, dst, dst, tmp);
    }
}

/* MOVE-family CCR update: N,Z from the value in `vreg`; V,C cleared;
 * X preserved. `vreg` is not clobbered. */
static void emit_logic_flags(xt_emit *e, u8 vreg) {
    xt_l16ui(e, 9, R_CPU, OFF_SR);
    xt_movi (e, 10, -16);               /* keep bit 4 (X) and up */
    xt_and  (e, 9, 9, 10);
    xt_extui(e, 11, vreg, 31, 0);       /* N bit */
    xt_slli (e, 11, 11, 3);
    xt_or   (e, 9, 9, 11);
    xt_movi (e, 11, 0x04);              /* Z */
    xt_bnez (e, vreg, 6);               /* value != 0 -> skip the OR */
    xt_or   (e, 9, 9, 11);
    xt_s16i (e, 9, R_CPU, OFF_SR);
}

/* MOVEQ #imm8,Dn — d[n] = sign-extend(imm8); MOVE-family flags. */
static void emit_moveq(xt_emit *e, u16 op) {
    int dn = (op >> 9) & 7;
    i32 imm = (i8)(op & 0xFF);
    xt_movi(e, 8, imm);
    xt_s32i(e, 8, R_CPU, OFF_D(dn));
    emit_logic_flags(e, 8);
    emit_advance(e, 2, 4);
}

/* MOVE.L #imm32,Dn. */
static void emit_move_l_imm_dn(xt_emit *e, int dn, u32 imm) {
    emit_load_imm32(e, 8, 15, imm);
    xt_s32i(e, 8, R_CPU, OFF_D(dn));
    emit_logic_flags(e, 8);
    emit_advance(e, 6, 8);
}

/* MOVEA.L #imm32,An — address registers take no flags. */
static void emit_movea_l_imm(xt_emit *e, int an, u32 imm) {
    emit_load_imm32(e, 8, 15, imm);
    xt_s32i(e, 8, R_CPU, OFF_A(an));
    emit_advance(e, 6, 8);
}

/* MOVE.L Dm,Dn. */
static void emit_move_l_dd(xt_emit *e, int dn, int dm) {
    xt_l32i(e, 8, R_CPU, OFF_D(dm));
    xt_s32i(e, 8, R_CPU, OFF_D(dn));
    emit_logic_flags(e, 8);
    emit_advance(e, 2, 8);
}

/* Emit the CCR update shared by the inlined long ADD/SUB family.
 * Convention on entry: a8 = source, a9 = dest, a10 = result (already
 * stored). Clobbers a8..a13; writes the five CCR bits into cpu->sr.
 *
 * Bit identities used (msb = bit 31):
 *   add carry  out = maj(s, d, ~r)        = (s&d) | ((s|d) & ~r)
 *   sub borrow out = maj(~d, s, r)        = (~d&s) | ((~d|s) & r)
 *   add overflow   = (s^r) & (d^r)
 *   sub overflow   = (s^d) & (d^r)
 *   N = r msb, Z = (r == 0), X = C. */
static void emit_addsub_flags_long(xt_emit *e, bool is_sub, bool keep_x) {
    if (!is_sub) {
        xt_and(e, 11, 8, 9);            /* s & d            */
        xt_or (e, 12, 8, 9);            /* s | d            */
        xt_movi(e, 13, -1);
        xt_xor(e, 13, 10, 13);          /* ~r               */
    } else {
        xt_movi(e, 13, -1);
        xt_xor(e, 13, 9, 13);           /* ~d               */
        xt_and(e, 11, 13, 8);           /* ~d & s           */
        xt_or (e, 12, 13, 8);           /* ~d | s           */
        xt_mov(e, 13, 10);              /* r                */
    }
    xt_and (e, 12, 12, 13);             /* (s|d)&~r  /  (~d|s)&r */
    xt_or  (e, 11, 11, 12);             /* carry/borrow term     */
    xt_extui(e, 11, 11, 31, 1);         /* a11 = C (0/1)         */

    if (!is_sub) xt_xor(e, 12, 8, 10);  /* s ^ r */
    else         xt_xor(e, 12, 8, 9);   /* s ^ d */
    xt_xor (e, 13, 9, 10);              /* d ^ r */
    xt_and (e, 12, 12, 13);
    xt_extui(e, 12, 12, 31, 1);         /* a12 = V */

    xt_extui(e, 13, 10, 31, 1);         /* a13 = N */

    /* ccr = C | V<<1 | N<<3 [ | X<<4 ]   (X == C for ADD/SUB; CMP keeps X) */
    xt_slli(e, 12, 12, 1);
    xt_or  (e, 8, 11, 12);
    xt_slli(e, 13, 13, 3);
    xt_or  (e, 8, 8, 13);
    if (!keep_x) {
        xt_slli(e, 9, 11, 4);
        xt_or  (e, 8, 8, 9);
    }
    /* Z: OR in 0x04 only when the result is zero. */
    xt_movi(e, 9, 0x04);
    xt_bnez(e, 10, 6);                  /* r != 0 -> skip the next op */
    xt_or  (e, 8, 8, 9);
    /* sr = (sr & mask) | ccr  — mask clears the bits we are about to set. */
    xt_l16ui(e, 9, R_CPU, OFF_SR);
    xt_movi (e, 12, keep_x ? -16 : -32);  /* ~0x0F (keep X) or ~0x1F */
    xt_and  (e, 9, 9, 12);
    xt_or   (e, 9, 9, 8);
    xt_s16i (e, 9, R_CPU, OFF_SR);
}

/* ADD.L Dm,Dn — d[dn] += d[dm], full CCR. */
static void emit_add_l_dd(xt_emit *e, int dn, int dm) {
    xt_l32i(e, 8, R_CPU, OFF_D(dm));
    xt_l32i(e, 9, R_CPU, OFF_D(dn));
    xt_add (e, 10, 8, 9);
    xt_s32i(e, 10, R_CPU, OFF_D(dn));
    emit_addsub_flags_long(e, false, false);
    emit_advance(e, 2, 8);
}

/* ADDQ.L / SUBQ.L #imm,Dn — d[dn] +/- imm (imm 1..8), full CCR. */
static void emit_addq_l_dd(xt_emit *e, int dn, int imm, bool is_sub) {
    xt_movi(e, 8, imm);                 /* source operand */
    xt_l32i(e, 9, R_CPU, OFF_D(dn));
    if (is_sub) xt_sub(e, 10, 9, 8);
    else        xt_add(e, 10, 8, 9);
    xt_s32i(e, 10, R_CPU, OFF_D(dn));
    emit_addsub_flags_long(e, is_sub, false);
    emit_advance(e, 2, 8);
}

/* ADDQ / SUBQ #imm,An — address-register form: no flags, any size. */
static void emit_addq_an(xt_emit *e, int an, int delta) {
    xt_l32i(e, 8, R_CPU, OFF_A(an));
    xt_addi(e, 8, 8, delta);            /* delta in -8..8 */
    xt_s32i(e, 8, R_CPU, OFF_A(an));
    emit_advance(e, 2, 8);
}

/* CMP.L Dm,Dn — compares (Dn - Dm); sets N/Z/V/C, leaves X and the
 * registers untouched. */
static void emit_cmp_l_dd(xt_emit *e, int dn, int dm) {
    xt_l32i(e, 8, R_CPU, OFF_D(dm));     /* s */
    xt_l32i(e, 9, R_CPU, OFF_D(dn));     /* d */
    xt_sub (e, 10, 9, 8);               /* r = d - s (discarded) */
    emit_addsub_flags_long(e, true, true);
    emit_advance(e, 2, 8);
}

/* --- block compiler --------------------------------------------------- */

m68k_block *m68k_compile_block(codecache *cc, m68k_cpu *cpu, u32 pc,
                               jit_helper_addr_fn helper_addr, void *user) {
    /* 1. Walk the instruction stream to discover the block extent. */
    u16 op_word[M68K_MAX_OPS_PER_BLOCK];
    u32 op_pc  [M68K_MAX_OPS_PER_BLOCK];
    u32 n_ops = 0;
    u32 cur = pc;
    for (;;) {
        if (n_ops >= M68K_MAX_OPS_PER_BLOCK) break;
        m68k_decoded d = m68k_decode_at(cpu, cur);
        op_word[n_ops] = d.opcode;
        op_pc[n_ops] = cur;
        n_ops++;
        cur += d.length;
        if (d.ends_block) break;
    }
    if (n_ops == 0) return NULL;

    /* 2. Reserve a worst-case span from the codecache. */
    u32 lit_bytes = align_up_4((u32)LITERAL_COUNT * 4u);
    u32 total = lit_bytes + PROLOGUE_EPILOGUE + n_ops * BYTES_PER_OP;
    total = align_up_4(total);
    u8 *base = codecache_alloc(cc, total);
    if (!base) return NULL;
    /* Word-wise clear — never a byte store: this buffer is IRAM-resident
     * executable memory on the ESP32-S3, where 8-bit accesses fault with
     * LoadStoreError. `total` is 4-aligned and so is `base`. */
    for (u32 i = 0; i < total; i += 4) *(u32 *)(base + i) = 0;

    /* 3. Literal pool. */
    u32 lit_off[LITERAL_COUNT];
    for (u32 i = 0; i < LITERAL_COUNT; i++) {
        lit_off[i] = i * 4u;
        *(u32 *)(base + i * 4u) = helper_addr((literal_id)i, user);
    }
    u32 entry_off = lit_bytes;

    xt_emit e;
    xt_init(&e, base + entry_off, total - entry_off);

    /* 4. Prologue: a3 = cpu_state base; stash the CALL0 return PC. */
    emit_l32r_at(&e, R_CPU, lit_off[ADDR_CPU_BASE], entry_off + e.len);
    xt_s32i(&e, 0, R_CPU, OFF_JITRETPC);

    /* 5. Body. */
    u32 inline_ops = 0, helper_ops = 0;
    for (u32 i = 0; i < n_ops; i++) {
        u16 w = op_word[i];
        int top = (w >> 12) & 0xF;
        bool done = false;

        int szf  = (w >> 6) & 3;
        int mode = (w >> 3) & 7;

        if (top == 0x7) {                  /* MOVEQ */
            emit_moveq(&e, w);
            inline_ops++; done = true;
        } else if (w == 0x4E71) {          /* NOP */
            emit_advance(&e, 2, 4);
            inline_ops++; done = true;
        } else if (top == 0xD && szf == 2 && !((w >> 8) & 1) && mode == 0) {
            /* ADD.L Dm,Dn */
            emit_add_l_dd(&e, (w >> 9) & 7, w & 7);
            inline_ops++; done = true;
        } else if (top == 0x5 && szf == 2 && mode == 0) {
            /* ADDQ.L / SUBQ.L #imm,Dn */
            int data = (w >> 9) & 7; if (data == 0) data = 8;
            emit_addq_l_dd(&e, w & 7, data, (w >> 8) & 1);
            inline_ops++; done = true;
        } else if (top == 0x5 && szf != 3 && mode == 1) {
            /* ADDQ / SUBQ #imm,An */
            int data = (w >> 9) & 7; if (data == 0) data = 8;
            emit_addq_an(&e, w & 7, ((w >> 8) & 1) ? -data : data);
            inline_ops++; done = true;
        } else if (top == 0x2 && ((w >> 3) & 7) == 7 && (w & 7) == 4) {
            /* MOVE.L / MOVEA.L #imm32,<dst>  (src = immediate). */
            u32 imm = mac_read32(cpu->mem, op_pc[i] + 2);
            int dst_mode = (w >> 6) & 7, dst_reg = (w >> 9) & 7;
            if (dst_mode == 0) {
                emit_move_l_imm_dn(&e, dst_reg, imm);
                inline_ops++; done = true;
            } else if (dst_mode == 1) {
                emit_movea_l_imm(&e, dst_reg, imm);
                inline_ops++; done = true;
            }
        } else if (top == 0x2 && mode == 0 && ((w >> 6) & 7) == 0) {
            /* MOVE.L Dm,Dn */
            emit_move_l_dd(&e, (w >> 9) & 7, w & 7);
            inline_ops++; done = true;
        } else if (top == 0xB && szf == 2 && !((w >> 8) & 1) && mode == 0) {
            /* CMP.L Dm,Dn */
            emit_cmp_l_dd(&e, (w >> 9) & 7, w & 7);
            inline_ops++; done = true;
        }

        if (!done) {
            /* Helper fallback: m68k_step(cpu). a3 survives the call. */
            xt_mov(&e, R_ARG, R_CPU);
            emit_l32r_at(&e, R_HELP, lit_off[HELPER_M68K_STEP], entry_off + e.len);
            xt_callx0(&e, R_HELP);
            helper_ops++;
        }
    }

    /* 6. Epilogue: restore return PC and jump back to the dispatcher. */
    xt_l32i(&e, 0, R_CPU, OFF_JITRETPC);
    xt_jx(&e, 0);

    if (e.overflow) {
        codecache_free(cc, (u32)(base - cc->base), total);
        return NULL;
    }
    xt_flush_pending(&e);
    codecache_finalize(cc, base + entry_off, e.len);

    u32 actual = align_up_4(entry_off + e.len);
    codecache_trim(cc, base, actual, total);

    m68k_block *b = (m68k_block *)calloc(1, sizeof(*b));
    if (!b) {
        codecache_free(cc, (u32)(base - cc->base), actual);
        return NULL;
    }
    b->pc_start = pc;
    b->pc_end = cur;
    b->n_ops = n_ops;
    b->code = base;
    b->code_size = actual;
    b->entry_off = entry_off;
    b->inline_ops = inline_ops;
    b->helper_ops = helper_ops;
    b->predicted_next = NULL;
    b->predicted_next_pc = 0xFFFFFFFFu;
    b->hash_next = NULL;
    return b;
}

void m68k_block_free(m68k_block *b) {
    free(b);
}
