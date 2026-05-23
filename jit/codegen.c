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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* cpu_state field byte offsets — resolved by the compiler, never guessed. */
#define OFF_D(n)     ((u32)(offsetof(m68k_cpu, d) + (n) * 4u))
#define OFF_A(n)     ((u32)(offsetof(m68k_cpu, a) + (n) * 4u))
#define OFF_PC       ((u32)offsetof(m68k_cpu, pc))
#define OFF_SR       ((u32)offsetof(m68k_cpu, sr))
#define OFF_CYCLES   ((u32)offsetof(m68k_cpu, cycles))   /* low 32 bits */
#define OFF_JITRETPC ((u32)offsetof(m68k_cpu, jit_ret_pc))
#define OFF_JIT_ARG1 ((u32)offsetof(m68k_cpu, jit_arg1))
#define OFF_JIT_ARG2 ((u32)offsetof(m68k_cpu, jit_arg2))

/* Xtensa registers reserved across a block. */
#define R_ARG   2     /* CALLX0 first argument          */
#define R_CPU   3     /* cpu_state base (survives calls) */
#define R_CACHE0 4    /* Four guest-register cache slots (a4..a7). The   */
#define R_CACHE1 5    /* per-block setup assigns hot D/A regs to these   */
#define R_CACHE2 6    /* slots, prologue loads them from cpu_state, the  */
#define R_CACHE3 7    /* epilogue (and any helper boundary) stores back. */
#define R_HELP 13     /* CALLX0 target scratch (was a14; freed for R_SR)  */
#define R_SR   14     /* cached cpu->sr (low 16 bits) — modified in place */
                      /* by flag emits; flushed to cpu_state around helpers. */
/* a8..a12 are inline-op scratch; a15 is emit_load_imm32 tmp. */

/* Cache state passed through every inline emit. cache_xt_reg[guest] returns
 * the Xtensa register caching guest reg `guest` (encoded as 0..7 for D0..D7,
 * 8..15 for A0..A7), or -1 if not cached. */
#define G_D(n) ((n) & 7)
#define G_A(n) (8 + ((n) & 7))
typedef struct {
    i8  xt[16];        /* xt[gi] = Xtensa register caching guest gi, or -1 */
    u8  guest[4];      /* slot i caches this guest reg (G_D/G_A encoding)  */
    u8  active;        /* number of slots in use (0..4)                    */
    u16 dirty;         /* bit i set if cache slot i is dirty (write-back   */
                       /* deferred). Cleared on flush.                     */
} regcache;

/* Worst-case bytes any single instruction's emission can need. The fattest
 * inline body (ADD.L with full flag computation) is ~40 narrow-form ops at
 * 3 bytes each. */
#define BYTES_PER_OP        240u
#define PROLOGUE_EPILOGUE   96u

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

/* --- register cache plumbing ------------------------------------------ */

static inline u8 cache_xt_slot(int slot) {
    return (u8)(R_CACHE0 + slot);   /* a4 + slot */
}

/* Returns the Xtensa register holding guest reg `gi` (G_D / G_A encoding),
 * or -1 if `gi` isn't cached. */
static inline int cache_lookup(const regcache *rc, int gi) {
    return rc ? rc->xt[gi] : -1;
}

/* Load the value of guest reg `gi` into the named Xtensa register `dst`.
 * Uses xt_mov from the cache slot when the reg is cached; falls back to
 * l32i from cpu_state otherwise. Idempotent — safe to call inside any emit. */
static void emit_read_g(xt_emit *e, const regcache *rc, int gi, u8 dst) {
    int xr = cache_lookup(rc, gi);
    if (xr >= 0) {
        if ((u8)xr != dst) xt_mov(e, dst, (u8)xr);
        return;
    }
    if (gi < 8) xt_l32i(e, dst, R_CPU, OFF_D(gi));
    else        xt_l32i(e, dst, R_CPU, OFF_A(gi - 8));
}

/* Return an Xtensa register holding the value of guest reg `gi`. If `gi`
 * is cached, returns the cache slot directly and emits nothing (caller
 * must treat the result as read-only — writing to it without dirty-marking
 * would desync the cache). Otherwise, emits l32i into `scratch` and
 * returns `scratch`.
 *
 * Use this in inline emits whose consumer reads but does not write to the
 * operand (the common case for ALU sources, bounds-check operands, etc.).
 * Saves the xt_mov from cache slot → scratch in the cached case. */
static u8 emit_read_g_in(xt_emit *e, const regcache *rc, int gi, u8 scratch) {
    int xr = cache_lookup(rc, gi);
    if (xr >= 0) return (u8)xr;
    if (gi < 8) xt_l32i(e, scratch, R_CPU, OFF_D(gi));
    else        xt_l32i(e, scratch, R_CPU, OFF_A(gi - 8));
    return scratch;
}

/* Write `src` into guest reg `gi`. Cached: mov to the cache slot and mark
 * it dirty (a later flush is responsible for the s32i). Uncached: s32i. */
static void emit_write_g(xt_emit *e, regcache *rc, int gi, u8 src) {
    int xr = cache_lookup(rc, gi);
    if (xr >= 0) {
        if ((u8)xr != src) xt_mov(e, (u8)xr, src);
        for (int i = 0; i < (rc ? rc->active : 0); i++)
            if (rc->guest[i] == (u8)gi) { rc->dirty |= (u16)(1u << i); break; }
        return;
    }
    if (gi < 8) xt_s32i(e, src, R_CPU, OFF_D(gi));
    else        xt_s32i(e, src, R_CPU, OFF_A(gi - 8));
}

/* Write back dirty cache slots to cpu state, then mark clean. */
static void emit_cache_flush(xt_emit *e, regcache *rc) {
    if (!rc) return;
    for (int i = 0; i < rc->active; i++) {
        if (!(rc->dirty & (1u << i))) continue;
        int gi = rc->guest[i];
        if (gi < 8) xt_s32i(e, cache_xt_slot(i), R_CPU, OFF_D(gi));
        else        xt_s32i(e, cache_xt_slot(i), R_CPU, OFF_A(gi - 8));
    }
    rc->dirty = 0;
}

/* Reload every cached slot from cpu state. Called after a helper. */
static void emit_cache_reload(xt_emit *e, regcache *rc) {
    if (!rc) return;
    for (int i = 0; i < rc->active; i++) {
        int gi = rc->guest[i];
        if (gi < 8) xt_l32i(e, cache_xt_slot(i), R_CPU, OFF_D(gi));
        else        xt_l32i(e, cache_xt_slot(i), R_CPU, OFF_A(gi - 8));
    }
    rc->dirty = 0;
}

/* --- inline op emitters ----------------------------------------------- */

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

/* Load `val` into `dst`. Uses a single xt_movi when val fits in 12-bit
 * signed (-2048..2047), else falls back to emit_load_imm32. Saves 9
 * Xtensa ops per call when the immediate is small. */
static void emit_load_imm(xt_emit *e, u8 dst, u8 tmp, u32 val) {
    i32 sv = (i32)val;
    if (sv >= -2048 && sv <= 2047) xt_movi(e, dst, sv);
    else                            emit_load_imm32(e, dst, tmp, val);
}

/* emit_advance is now compile-time bookkeeping only: it accumulates
 * pc/cyc deltas across consecutive inline ops, then a single combined
 * emit_advance_flush() emits the actual l32i/addi/s32i sequence before
 * each helper-step (so m68k_step sees current cpu->pc/cycles) and at
 * the block epilogue. Saves 6 ops per inline-op-between-helpers — big
 * win for inline-dominated hot loops where today every emit_advance
 * costs 6 ops × N inline ops per block. */
static i32 g_pc_acc;
static i32 g_cyc_acc;

/* Emit `cpu->pc += g_pc_acc; cpu->cycles += g_cyc_acc;` if either is
 * non-zero, then reset. */
static void emit_advance_flush(xt_emit *e) {
    if (g_pc_acc) {
        xt_l32i(e, 8, R_CPU, OFF_PC);
        if (g_pc_acc >= -128 && g_pc_acc <= 127) {
            xt_addi(e, 8, 8, g_pc_acc);
        } else {
            emit_load_imm(e, 9, 10, (u32)g_pc_acc);
            xt_add(e, 8, 8, 9);
        }
        xt_s32i(e, 8, R_CPU, OFF_PC);
        g_pc_acc = 0;
    }
    if (g_cyc_acc) {
        xt_l32i(e, 8, R_CPU, OFF_CYCLES);
        if (g_cyc_acc >= -128 && g_cyc_acc <= 127) {
            xt_addi(e, 8, 8, g_cyc_acc);
        } else {
            emit_load_imm(e, 9, 10, (u32)g_cyc_acc);
            xt_add(e, 8, 8, 9);
        }
        xt_s32i(e, 8, R_CPU, OFF_CYCLES);
        g_cyc_acc = 0;
    }
}

/* Per-op call. Just accumulates; the deferred flush emits the code. */
static void emit_advance(xt_emit *e, i32 pc_delta, i32 cyc) {
    (void)e;
    g_pc_acc += pc_delta;
    g_cyc_acc += cyc;
}

/* (emit_advance_now was used to directly emit cpu->pc/cycles updates
 * in conditional-helper fast paths. The deferred-advance refactor in
 * M6.20 replaced it with emit_advance (compile-time accumulator) plus
 * emit_helper_step_after_flush_undo for the helper bridge — saves 6
 * LX7 ops per fast-path execution.) */

/* SR cache: cpu->sr lives in R_SR across the block. Modifying SR via
 * flag emits is now register-resident — no l16ui/s16i per emit. Helpers
 * read/write cpu->sr (interrupt level, X bit, etc.) so flush/reload
 * around any callx0 to m68k_step.
 *
 * `g_sr_dirty` is a per-block-compile flag: set true after every flag
 * emit, cleared by sr_flush. When false the flush is skipped (helper
 * still gets correct cpu->sr because no flag emit has touched R_SR
 * since the last flush/reload). Big win for helper-heavy boot code
 * with few inline flag emits. */
static bool g_sr_dirty;

/* Per-block compile-time memoization of sext(.W) results in a13.
 *   `g_sext_src_reg`: guest reg whose .W was sign-extended to 32 bits.
 *   `g_sext_valid`  : true if a13 currently holds that sext result.
 * Invalidated on: helper-step (callx0 sets a13 from HELPER literal),
 * flag emit (a13 used as scratch), emit_cond (a13 = N bit), any write
 * to `g_sext_src_reg`. Reset at start of each block. Saves 3 ops per
 * reused ADDA.W execution. */
static int  g_sext_src_reg;
static bool g_sext_valid;

/* Per-block PC-constant literal. compile_block writes the block
 * terminator's PC constant (Bcc.S fall-through `ft` or BRA.S `taken`)
 * to LITERAL_BCC_PC and exposes the slot via these globals. Each
 * emit_bcc_branchless_tail / emit_branch BRA load then uses a 1-op
 * `l32r` instead of the 10-op `emit_load_imm32`. Saves ~9 ops per
 * branch execution — for bench's fused CMP+Bcc the dominant tail
 * cost. `g_pc_lit_valid` is false when the literal is unset (block
 * has no PC-overwriting terminator). */
static bool g_pc_lit_valid;
static u32  g_pc_lit_val;
static u32  g_pc_lit_off;
static u32  g_pc_lit_entry_off;
static inline void sext_memo_invalidate(void) { g_sext_valid = false; }

static void emit_sr_flush(xt_emit *e) {
    if (g_sr_dirty) { xt_s16i(e, R_SR, R_CPU, OFF_SR); g_sr_dirty = false; }
}
static void emit_sr_reload(xt_emit *e) {
    xt_l16ui(e, R_SR, R_CPU, OFF_SR);
    g_sr_dirty = false;
}

static void emit_helper_step(xt_emit *e, u32 helper_lit_off, u32 entry_off,
                             regcache *rc) {
    emit_advance_flush(e);
    emit_cache_flush(e, rc);
    emit_sr_flush(e);
    xt_mov(e, R_ARG, R_CPU);
    emit_l32r_at(e, R_HELP, helper_lit_off, entry_off + e->len);
    xt_callx0(e, R_HELP);
    emit_sr_reload(e);
    emit_cache_reload(e, rc);
    sext_memo_invalidate();
}

/* (emit_helper_step_after_flush was the no-undo variant; replaced by
 * emit_helper_step_after_flush_undo at every conditional helper site.) */

/* Byte size of the conditional-helper bridge before the optional undo
 * is added. Used by helper_step_after_flush_undo_size. */
static u32 helper_step_after_flush_base_size(const regcache *rc) {
    u32 sz = 3u /*mov*/ + 3u /*l32r*/ + 3u /*callx0*/ + 3u /*sr_reload*/;
    if (g_sr_dirty) sz += 3u;
    if (!rc) return sz;
    sz += (u32)rc->active * 3u;
    return sz;
}

/* Variant that subtracts the op's own pc_delta/cyc from cpu->pc/cycles
 * AFTER the m68k_step's natural advance. Used when the conditional
 * helper site's fast path defers its own emit_advance_now into the
 * PC/cycles accumulator (saving 6 emitted LX7 ops per fast-path
 * execution at the cost of 6 emitted ops in the helper bridge). */
static void emit_helper_step_after_flush_undo(xt_emit *e, u32 helper_lit_off,
                                              u32 entry_off, regcache *rc,
                                              i32 pc_undo, i32 cyc_undo) {
    emit_sr_flush(e);
    xt_mov(e, R_ARG, R_CPU);
    emit_l32r_at(e, R_HELP, helper_lit_off, entry_off + e->len);
    xt_callx0(e, R_HELP);
    if (pc_undo) {
        xt_l32i(e, 8, R_CPU, OFF_PC);
        xt_addi(e, 8, 8, -pc_undo);
        xt_s32i(e, 8, R_CPU, OFF_PC);
    }
    if (cyc_undo) {
        xt_l32i(e, 8, R_CPU, OFF_CYCLES);
        xt_addi(e, 8, 8, -cyc_undo);
        xt_s32i(e, 8, R_CPU, OFF_CYCLES);
    }
    emit_sr_reload(e);
    emit_cache_reload(e, rc);
    sext_memo_invalidate();   /* helper branch sets a13 = HELPER literal */
}
static u32 helper_step_after_flush_undo_size(const regcache *rc,
                                             i32 pc_undo, i32 cyc_undo) {
    u32 sz = helper_step_after_flush_base_size(rc);
    if (pc_undo) sz += 9u;
    if (cyc_undo) sz += 9u;
    return sz;
}

/* Emit a "fast helper" bridge for ops that pass (jit_arg1, jit_arg2)
 * via cpu state and call a specialised C helper instead of m68k_step.
 * The helper does NOT touch cpu->pc / cpu->cycles — the JIT arm's own
 * emit_advance handles that.
 *
 * a8 must hold the value to store into jit_arg1 (e.g. the address or
 * the reglist bitmap). `imm2` is the constant to store into jit_arg2
 * (small enough for xt_movi: -2048..2047). */
static void emit_jit_fast_helper(xt_emit *e, u8 a8_arg1_reg, i32 imm2,
                                 u32 helper_lit_off, u32 entry_off,
                                 regcache *rc) {
    sext_memo_invalidate();
    emit_sr_flush(e);
    xt_s32i(e, a8_arg1_reg, R_CPU, OFF_JIT_ARG1);
    xt_movi(e, 9, imm2);
    xt_s32i(e, 9, R_CPU, OFF_JIT_ARG2);
    xt_mov (e, R_ARG, R_CPU);
    emit_l32r_at(e, R_HELP, helper_lit_off, entry_off + e->len);
    xt_callx0(e, R_HELP);
    emit_sr_reload(e);
    emit_cache_reload(e, rc);
}

/* Size of an emit_load_imm(list) + emit_jit_fast_helper sequence —
 * what the MOVEM small-N arms use to replace their m68k_step bridge
 * with a fast-helper bridge. */
static u32 emit_movem_fast_bridge_size(u32 list, const regcache *rc);

/* Size in bytes (must match emit_jit_fast_helper exactly). */
static u32 emit_jit_fast_helper_size(const regcache *rc) {
    u32 sz = 3u /*s32i arg1*/ + 3u /*movi*/ + 3u /*s32i arg2*/
           + 3u /*mov R_ARG*/ + 3u /*l32r*/ + 3u /*callx0*/
           + 3u /*sr_reload*/;
    if (g_sr_dirty) sz += 3u;
    if (rc) sz += (u32)rc->active * 3u;
    return sz;
}

static u32 emit_movem_fast_bridge_size(u32 list, const regcache *rc) {
    u32 sz = 0;
    i32 sv = (i32)list;
    if (sv >= -2048 && sv <= 2047) sz += 3u;  /* xt_movi */
    else sz += 30u;                            /* emit_load_imm32: 10 ops */
    sz += emit_jit_fast_helper_size(rc);
    return sz;
}

/* MOVE-family CCR update: N,Z from the value in `vreg`; V,C cleared;
 * X preserved. `vreg` is not clobbered. R_SR is updated in place. */
static void emit_logic_flags(xt_emit *e, u8 vreg) {
    xt_movi (e, 10, -16);               /* keep bit 4 (X) and up */
    xt_and  (e, R_SR, R_SR, 10);
    xt_extui(e, 11, vreg, 31, 0);       /* N bit */
    xt_slli (e, 11, 11, 3);
    xt_or   (e, R_SR, R_SR, 11);
    xt_movi (e, 11, 0x04);              /* Z */
    xt_bnez (e, vreg, 6);               /* value != 0 -> skip the OR */
    xt_or   (e, R_SR, R_SR, 11);
    g_sr_dirty = true;
    sext_memo_invalidate();
}

/* MOVEQ #imm8,Dn — d[n] = sign-extend(imm8); MOVE-family flags. */
static void emit_moveq(xt_emit *e, u16 op, bool skip_flags, regcache *rc) {
    int dn = (op >> 9) & 7;
    i32 imm = (i8)(op & 0xFF);
    int xt_dst = cache_lookup(rc, G_D(dn));
    u8 dst = (xt_dst >= 0) ? (u8)xt_dst : 8;
    xt_movi(e, dst, imm);
    if (xt_dst >= 0) {
        for (int i = 0; i < rc->active; i++)
            if (rc->guest[i] == (u8)G_D(dn)) { rc->dirty |= (u16)(1u << i); break; }
    } else {
        xt_s32i(e, dst, R_CPU, OFF_D(dn));
    }
    if (!skip_flags) emit_logic_flags(e, dst);
    emit_advance(e, 2, 4);
}

/* MOVE.L #imm32,Dn. */
static void emit_move_l_imm_dn(xt_emit *e, int dn, u32 imm, bool skip_flags, regcache *rc) {
    int xt_dst = cache_lookup(rc, G_D(dn));
    u8 dst = (xt_dst >= 0) ? (u8)xt_dst : 8;
    emit_load_imm(e, dst, 10, imm);
    if (xt_dst >= 0) {
        for (int i = 0; i < rc->active; i++)
            if (rc->guest[i] == (u8)G_D(dn)) { rc->dirty |= (u16)(1u << i); break; }
    } else {
        xt_s32i(e, dst, R_CPU, OFF_D(dn));
    }
    if (!skip_flags) emit_logic_flags(e, dst);
    emit_advance(e, 6, 8);
}

/* MOVEA.L #imm32,An — address registers take no flags. */
static void emit_movea_l_imm(xt_emit *e, int an, u32 imm, regcache *rc) {
    int xt_dst = cache_lookup(rc, G_A(an));
    u8 dst = (xt_dst >= 0) ? (u8)xt_dst : 8;
    emit_load_imm(e, dst, 10, imm);
    if (xt_dst >= 0) {
        for (int i = 0; i < rc->active; i++)
            if (rc->guest[i] == (u8)G_A(an)) { rc->dirty |= (u16)(1u << i); break; }
    } else {
        xt_s32i(e, dst, R_CPU, OFF_A(an));
    }
    emit_advance(e, 6, 8);
}

/* MOVE.L Dm,Dn. */
static void emit_move_l_dd(xt_emit *e, int dn, int dm, bool skip_flags, regcache *rc) {
    /* When both regs are cached we can mov cache_dn ← cache_dm and skip
     * both the l32i and the s32i. */
    int xt_dst = cache_lookup(rc, G_D(dn));
    u8 dst = (xt_dst >= 0) ? (u8)xt_dst : 8;
    emit_read_g(e, rc, G_D(dm), dst);
    if (xt_dst >= 0) {
        for (int i = 0; i < rc->active; i++)
            if (rc->guest[i] == (u8)G_D(dn)) { rc->dirty |= (u16)(1u << i); break; }
    } else {
        xt_s32i(e, dst, R_CPU, OFF_D(dn));
    }
    if (!skip_flags) emit_logic_flags(e, dst);
    emit_advance(e, 2, 8);
}

/* MOVEA.L <Dm|Am>,An — a[an] = src register (full 32 bits). No flags. */
static void emit_movea_l_reg(xt_emit *e, int an, int src, bool src_is_an, regcache *rc) {
    int g_src = src_is_an ? G_A(src) : G_D(src);
    int g_dst = G_A(an);
    int xt_src = cache_lookup(rc, g_src);
    int xt_dst = cache_lookup(rc, g_dst);
    if (xt_dst >= 0) {
        /* Write directly into the cache slot for An, skip the s32i. */
        if (xt_src >= 0) {
            if ((u8)xt_src != (u8)xt_dst) xt_mov(e, (u8)xt_dst, (u8)xt_src);
        } else {
            if (g_src < 8) xt_l32i(e, (u8)xt_dst, R_CPU, OFF_D(g_src));
            else           xt_l32i(e, (u8)xt_dst, R_CPU, OFF_A(g_src - 8));
        }
        for (int i = 0; i < rc->active; i++)
            if (rc->guest[i] == (u8)g_dst) { rc->dirty |= (u16)(1u << i); break; }
    } else {
        emit_read_g(e, rc, g_src, 8);
        xt_s32i(e, 8, R_CPU, OFF_A(an));
    }
    emit_advance(e, 2, 8);
}

/* ADDA.W / SUBA.W #imm16,An — a[an] ± sign-extend(imm16). No CCR.
 * Boot-hot 0xD0FC (ADDA.W #imm,A0) at 131 K execs / 60 M cycles. */
static void emit_adda_w_imm(xt_emit *e, int an, i16 imm, bool is_sub, regcache *rc) {
    i32 sext = (i32)imm;
    if (is_sub) sext = -sext;
    int xt_dst = cache_lookup(rc, G_A(an));
    if (xt_dst >= 0) {
        u8 dst = (u8)xt_dst;
        if (sext >= -128 && sext <= 127) {
            xt_addi(e, dst, dst, sext);
        } else {
            emit_load_imm(e, 9, 10, (u32)sext);
            xt_add(e, dst, dst, 9);
        }
        for (int i = 0; i < rc->active; i++)
            if (rc->guest[i] == (u8)G_A(an)) { rc->dirty |= (u16)(1u << i); break; }
    } else {
        xt_l32i(e, 8, R_CPU, OFF_A(an));
        if (sext >= -128 && sext <= 127) {
            xt_addi(e, 8, 8, sext);
        } else {
            emit_load_imm(e, 9, 10, (u32)sext);
            xt_add(e, 8, 8, 9);
        }
        xt_s32i(e, 8, R_CPU, OFF_A(an));
    }
    emit_advance(e, 4, 8);
}

/* ADDA.W <Dm|Am>,An — a[an] += sign-extend-word(src register). No CCR. */
static void emit_adda_w_reg(xt_emit *e, int an, int src, bool src_is_an, regcache *rc) {
    int g_src = src_is_an ? G_A(src) : G_D(src);
    int g_dst = G_A(an);
    /* Sign-extended .W of g_src may already be live in a13 from a recent
     * ADDA.W with the same source — saves 3 ops per reused execution. */
    if (!g_sext_valid || g_sext_src_reg != g_src) {
        emit_read_g(e, rc, g_src, 13);
        xt_slli(e, 13, 13, 16);
        xt_srai(e, 13, 13, 16);
        g_sext_src_reg = g_src;
        g_sext_valid = true;
    }
    int xt_dst = cache_lookup(rc, g_dst);
    if (xt_dst >= 0) {
        xt_add(e, (u8)xt_dst, (u8)xt_dst, 13);
        for (int i = 0; i < rc->active; i++)
            if (rc->guest[i] == (u8)g_dst) { rc->dirty |= (u16)(1u << i); break; }
    } else {
        xt_l32i(e, 9, R_CPU, OFF_A(an));
        xt_add (e, 9, 9, 13);
        xt_s32i(e, 9, R_CPU, OFF_A(an));
    }
    emit_advance(e, 2, 12);
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
/* CCR-bit mask passed to the flag emitters: bit 0 = C, 1 = V, 2 = Z,
 * 3 = N, 4 = X. The lazy-CC peephole uses this to skip computation of
 * bits the next consumer doesn't read. */
#define CCR_BIT_C  0x01u
#define CCR_BIT_V  0x02u
#define CCR_BIT_Z  0x04u
#define CCR_BIT_N  0x08u
#define CCR_BIT_X  0x10u
#define CCR_MASK_ALL 0x1Fu

/* Parameterised: `s`, `d`, `r` may be the conventional a8/a9/a10, or any
 * other register (cache slot a4..a7 for direct-cache emits). The function
 * uses a11..a13 as scratch and clobbers a8, a9 in the final CCR-pack
 * stage — callers must not rely on a8/a9 surviving the flag emit.
 *
 * `cc_mask` selects which CCR bits to materialise. Bits not in the mask
 * are left unchanged in R_SR (their previous value is preserved). Used
 * by the CMP-Bcc peephole: cc_mask = whatever_bits_bcc_reads. */
static void emit_addsub_flags_long_masked(xt_emit *e, bool is_sub, bool keep_x,
                                          u8 s, u8 d, u8 r, u8 cc_mask) {
    bool need_c = (cc_mask & CCR_BIT_C) != 0;
    bool need_v = (cc_mask & CCR_BIT_V) != 0;
    bool need_z = (cc_mask & CCR_BIT_Z) != 0;
    bool need_n = (cc_mask & CCR_BIT_N) != 0;
    bool need_x = !keep_x && need_c;     /* X == C for ADD/SUB writes */
    /* If nothing needs materialising, leave R_SR untouched. */
    if (!need_c && !need_v && !need_z && !need_n && !need_x) {
        return;
    }
    /* Full-mask fast path (matches the pre-refactor 22-op sequence). */
    bool full = need_c && need_v && need_z && need_n && (keep_x || need_x);
    if (full) {
        if (!is_sub) {
            xt_and(e, 11, s, d);
            xt_or (e, 12, s, d);
            xt_movi(e, 13, -1);
            xt_xor(e, 13, r, 13);
        } else {
            xt_movi(e, 13, -1);
            xt_xor(e, 13, d, 13);
            xt_and(e, 11, 13, s);
            xt_or (e, 12, 13, s);
            xt_mov(e, 13, r);
        }
        xt_and (e, 12, 12, 13);
        xt_or  (e, 11, 11, 12);
        xt_extui(e, 11, 11, 31, 1);
        if (!is_sub) xt_xor(e, 12, s, r);
        else         xt_xor(e, 12, s, d);
        xt_xor (e, 13, d, r);
        xt_and (e, 12, 12, 13);
        xt_extui(e, 12, 12, 31, 1);
        xt_extui(e, 13, r, 31, 1);
        xt_slli(e, 12, 12, 1);
        xt_or  (e, 8, 11, 12);
        xt_slli(e, 13, 13, 3);
        xt_or  (e, 8, 8, 13);
        if (!keep_x) { xt_slli(e, 9, 11, 4); xt_or(e, 8, 8, 9); }
        xt_movi(e, 9, 0x04);
        xt_bnez(e, r, 6);
        xt_or  (e, 8, 8, 9);
        xt_movi(e, 12, keep_x ? -16 : -32);
        xt_and (e, R_SR, R_SR, 12);
        xt_or  (e, R_SR, R_SR, 8);
        g_sr_dirty = true;
        return;
    }

    /* Compute C bit (a11 = 0 or 1) if needed. */
    if (need_c || need_x) {
        if (!is_sub) {
            xt_and(e, 11, s, d);
            xt_or (e, 12, s, d);
            xt_movi(e, 13, -1);
            xt_xor(e, 13, r, 13);
        } else {
            xt_movi(e, 13, -1);
            xt_xor(e, 13, d, 13);
            xt_and(e, 11, 13, s);
            xt_or (e, 12, 13, s);
            xt_mov(e, 13, r);
        }
        xt_and (e, 12, 12, 13);
        xt_or  (e, 11, 11, 12);
        xt_extui(e, 11, 11, 31, 1);     /* a11 = C */
    }
    if (need_v) {
        if (!is_sub) xt_xor(e, 12, s, r);
        else         xt_xor(e, 12, s, d);
        xt_xor (e, 13, d, r);
        xt_and (e, 12, 12, 13);
        xt_extui(e, 12, 12, 31, 1);     /* a12 = V */
    }
    if (need_n) {
        xt_extui(e, 13, r, 31, 1);      /* a13 = N */
    }

    /* Pack into a8 — start with first set bit (avoid an extra movi 0). */
    bool packed = false;
    if (need_c) { xt_mov(e, 8, 11); packed = true; }
    if (need_v) {
        xt_slli(e, 12, 12, 1);
        if (packed) xt_or(e, 8, 8, 12); else { xt_mov(e, 8, 12); packed = true; }
    }
    if (need_n) {
        xt_slli(e, 13, 13, 3);
        if (packed) xt_or(e, 8, 8, 13); else { xt_mov(e, 8, 13); packed = true; }
    }
    if (need_x) {
        xt_slli(e, 9, 11, 4);
        if (packed) xt_or(e, 8, 8, 9); else { xt_mov(e, 8, 9); packed = true; }
    }
    if (need_z) {
        if (!packed) { xt_movi(e, 8, 0); packed = true; }
        xt_movi(e, 9, 0x04);
        xt_bnez(e, r, 6);
        xt_or  (e, 8, 8, 9);
    }
    u32 mask = ~((u32)cc_mask | (need_x ? CCR_BIT_X : 0));
    xt_movi(e, 12, (i32)mask);
    xt_and (e, R_SR, R_SR, 12);
    xt_or  (e, R_SR, R_SR, 8);
    g_sr_dirty = true;
    sext_memo_invalidate();
}

/* Default: emit all five bits (preserves prior behaviour). */
static inline void emit_addsub_flags_long_ex(xt_emit *e, bool is_sub,
                                             bool keep_x, u8 s, u8 d, u8 r) {
    emit_addsub_flags_long_masked(e, is_sub, keep_x, s, d, r,
                                  keep_x ? (CCR_MASK_ALL & ~CCR_BIT_X)
                                         : CCR_MASK_ALL);
}

static inline void emit_addsub_flags_long(xt_emit *e, bool is_sub, bool keep_x) {
    emit_addsub_flags_long_ex(e, is_sub, keep_x, 8, 9, 10);
}

/* ADD.L Dm,Dn — d[dn] += d[dm], full CCR. `skip_flags` lets the lazy-CC
 * pass drop the flag emission when this op's CCR is overwritten before
 * any consumer reads it.
 *
 * Direct-cache fast path (both Dm and Dn cached, dm != dn): in-place
 * `add dn_slot, dn_slot, dm_slot` (1 op) replaces the classic 4-op
 * sequence (2 movs + add + write-back mov). The flag emit takes the
 * pre-add d via a saved a9 when flags aren't dead. */
static void emit_add_l_dd(xt_emit *e, int dn, int dm, bool skip_flags, regcache *rc) {
    int xt_dm = cache_lookup(rc, G_D(dm));
    int xt_dn = cache_lookup(rc, G_D(dn));
    if (xt_dm >= 0 && xt_dn >= 0 && dn != dm) {
        u8 dm_reg = (u8)xt_dm, dn_reg = (u8)xt_dn;
        if (!skip_flags) xt_mov(e, 9, dn_reg);     /* a9 = pre-add d */
        xt_add(e, dn_reg, dn_reg, dm_reg);          /* in-place add */
        for (int i = 0; i < rc->active; i++)
            if (rc->guest[i] == (u8)G_D(dn)) { rc->dirty |= (u16)(1u << i); break; }
        if (!skip_flags)
            emit_addsub_flags_long_ex(e, false, false, dm_reg, 9, dn_reg);
        emit_advance(e, 2, 8);
        return;
    }
    emit_read_g(e, rc, G_D(dm), 8);     /* s */
    emit_read_g(e, rc, G_D(dn), 9);     /* d */
    xt_add (e, 10, 8, 9);
    emit_write_g(e, rc, G_D(dn), 10);
    if (!skip_flags) emit_addsub_flags_long(e, false, false);
    emit_advance(e, 2, 8);
}

/* ADDQ.L / SUBQ.L #imm,Dn — d[dn] +/- imm (imm 1..8), full CCR. */
static void emit_addq_l_dd(xt_emit *e, int dn, int imm, bool is_sub, bool skip_flags, regcache *rc) {
    xt_movi(e, 8, imm);
    emit_read_g(e, rc, G_D(dn), 9);
    if (is_sub) xt_sub(e, 10, 9, 8);
    else        xt_add(e, 10, 8, 9);
    emit_write_g(e, rc, G_D(dn), 10);
    if (!skip_flags) emit_addsub_flags_long(e, is_sub, false);
    emit_advance(e, 2, 8);
}

/* ADDQ / SUBQ #imm,An — address-register form: no flags, any size. */
static void emit_addq_an(xt_emit *e, int an, int delta, regcache *rc) {
    int xt_dst = cache_lookup(rc, G_A(an));
    if (xt_dst >= 0) {
        xt_addi(e, (u8)xt_dst, (u8)xt_dst, delta);
        for (int i = 0; i < rc->active; i++)
            if (rc->guest[i] == (u8)G_A(an)) { rc->dirty |= (u16)(1u << i); break; }
    } else {
        xt_l32i(e, 8, R_CPU, OFF_A(an));
        xt_addi(e, 8, 8, delta);
        xt_s32i(e, 8, R_CPU, OFF_A(an));
    }
    emit_advance(e, 2, 8);
}

/* CMP.L Dm,Dn — compares (Dn - Dm); sets N/Z/V/C, leaves X and the
 * registers untouched. Direct-cache fast path: read both operands
 * in-place (no movs) and compute (Dn - Dm) into a10. */
static void emit_cmp_l_dd(xt_emit *e, int dn, int dm, regcache *rc, u8 cc_mask) {
    u8 dm_reg = emit_read_g_in(e, rc, G_D(dm), 8);
    u8 dn_reg = emit_read_g_in(e, rc, G_D(dn), 9);
    xt_sub (e, 10, dn_reg, dm_reg);
    emit_addsub_flags_long_masked(e, true, true, dm_reg, dn_reg, 10, cc_mask);
    emit_advance(e, 2, 8);
}

/* SUB.L Dm,Dn — d[dn] -= d[dm], full CCR (mirrors emit_add_l_dd). */
static void emit_sub_l_dd(xt_emit *e, int dn, int dm, bool skip_flags, regcache *rc) {
    int xt_dm = cache_lookup(rc, G_D(dm));
    int xt_dn = cache_lookup(rc, G_D(dn));
    if (xt_dm >= 0 && xt_dn >= 0 && dn != dm) {
        u8 dm_reg = (u8)xt_dm, dn_reg = (u8)xt_dn;
        if (!skip_flags) xt_mov(e, 9, dn_reg);     /* a9 = pre-sub d */
        xt_sub(e, dn_reg, dn_reg, dm_reg);
        for (int i = 0; i < rc->active; i++)
            if (rc->guest[i] == (u8)G_D(dn)) { rc->dirty |= (u16)(1u << i); break; }
        if (!skip_flags)
            emit_addsub_flags_long_ex(e, true, false, dm_reg, 9, dn_reg);
        emit_advance(e, 2, 8);
        return;
    }
    emit_read_g(e, rc, G_D(dm), 8);
    emit_read_g(e, rc, G_D(dn), 9);
    xt_sub (e, 10, 9, 8);
    emit_write_g(e, rc, G_D(dn), 10);
    if (!skip_flags) emit_addsub_flags_long(e, true, false);
    emit_advance(e, 2, 8);
}

/* AND.L / OR.L / EOR.L register form: d[dst] = d[ra] <op> d[rb], logic CCR.
 * `kind`: 0 = OR, 1 = AND, 2 = EOR. */
static void emit_logic_l_dd_kind(xt_emit *e, int ra, int rb, int dst,
                                 int kind, bool skip_flags, regcache *rc) {
    emit_read_g(e, rc, G_D(ra), 8);
    emit_read_g(e, rc, G_D(rb), 9);
    if      (kind == 0) xt_or (e, 8, 8, 9);
    else if (kind == 1) xt_and(e, 8, 8, 9);
    else                xt_xor(e, 8, 8, 9);
    emit_write_g(e, rc, G_D(dst), 8);
    if (!skip_flags) emit_logic_flags(e, 8);
    emit_advance(e, 2, 8);
}
/* Back-compat shim — preserves callers that only need AND.L / EOR.L. */
static void emit_logic_l_dd(xt_emit *e, int ra, int rb, int dst,
                            bool is_eor, regcache *rc) {
    emit_logic_l_dd_kind(e, ra, rb, dst, is_eor ? 2 : 1, false, rc);
}

/* ORI/ANDI/SUBI/ADDI/EORI/CMPI.L #imm32,Dn. `kind` is the 68000 type
 * field (bits 11-9): 0 OR, 1 AND, 2 SUB, 3 ADD, 5 EOR, 6 CMP. */
static void emit_immalu_l_dn(xt_emit *e, int dn, u32 imm, int kind, regcache *rc) {
    emit_load_imm(e, 8, 10, imm);
    emit_read_g(e, rc, G_D(dn), 9);
    if (kind == 3) {                         /* ADDI */
        xt_add (e, 10, 8, 9);
        emit_write_g(e, rc, G_D(dn), 10);
        emit_addsub_flags_long(e, false, false);
    } else if (kind == 2) {                  /* SUBI */
        xt_sub (e, 10, 9, 8);
        emit_write_g(e, rc, G_D(dn), 10);
        emit_addsub_flags_long(e, true, false);
    } else if (kind == 6) {                  /* CMPI — result discarded */
        xt_sub (e, 10, 9, 8);
        emit_addsub_flags_long(e, true, true);
    } else {                                 /* ORI / ANDI / EORI */
        if      (kind == 0) xt_or (e, 8, 8, 9);
        else if (kind == 1) xt_and(e, 8, 8, 9);
        else                xt_xor(e, 8, 8, 9);
        emit_write_g(e, rc, G_D(dn), 8);
        emit_logic_flags(e, 8);
    }
    emit_advance(e, 6, 8);
}

/* CMPI/ADDI/SUBI .W/.B #imm,Dn. `size`: 1 = .B, 2 = .W. `kind`:
 * 2=SUBI, 3=ADDI, 6=CMPI. ADDI/SUBI write back only the low `size`
 * bits of Dn, preserving the upper bits; CMPI is flag-only.
 *
 * emit_addsub_flags_long_ex reads (s, d, r) only in its first few
 * instructions, then uses a8/a9/a11..a13 as scratch. So we use the
 * conventional a8/a9/a10 = s'/d'/r' (shifted operands) — a10 is never
 * written by the flag emit, and a8/a9 are written only after all
 * operand reads complete. */
static void emit_immarith_bw_dn(xt_emit *e, int dn, u32 imm, int size,
                                int kind, regcache *rc) {
    u32 size_mask = (size == 1) ? 0xFFu : 0xFFFFu;
    i32 sext_imm = (i32)(imm & size_mask);
    if (size == 1) sext_imm = (i32)(i8)sext_imm;
    else           sext_imm = (i32)(i16)sext_imm;
    emit_load_imm(e, 8, 10, (u32)sext_imm);     /* a8 = sext_imm */
    emit_read_g(e, rc, G_D(dn), 9);              /* a9 = Dn */

    /* Compute the 32-bit result; only the low `size` bits are valid for
     * write-back, but the full 32-bit ADD/SUB is fine for the shift-up
     * trick on the flag side because (a+b) << shift == (a<<shift) + (b<<shift). */
    if (kind == 3) xt_add(e, 12, 9, 8);          /* a12 = Dn + imm */
    else           xt_sub(e, 12, 9, 8);          /* a12 = Dn - imm */

    if (kind != 6) {
        /* Merge: (Dn & ~size_mask) | (result & size_mask). */
        if (size == 1) {
            xt_movi(e, 11, -256);                /* a11 = 0xFFFFFF00 */
        } else {
            xt_movi(e, 11, -1);
            xt_slli(e, 11, 11, 16);              /* a11 = 0xFFFF0000 */
        }
        xt_and (e, 13, 9, 11);                   /* a13 = Dn & ~mask */
        xt_extui(e, 11, 12, 0, (size == 1) ? 7 : 15);  /* a11 = result & mask */
        xt_or  (e, 13, 13, 11);                  /* a13 = merged */
        emit_write_g(e, rc, G_D(dn), 13);
    }

    /* Shift s, d, r into the high bits for the flag emit. After this
     * we no longer need the raw a8/a9 values. */
    int shift = (size == 1) ? 24 : 16;
    xt_slli(e, 8,  8,  shift);                   /* s' */
    xt_slli(e, 9,  9,  shift);                   /* d' (overwrites Dn) */
    xt_slli(e, 10, 12, shift);                   /* r' */
    emit_addsub_flags_long_ex(e, kind == 2 || kind == 6, kind == 6,
                              8, 9, 10);
    emit_advance(e, 4, 8);
}

/* ORI/ANDI/EORI .W and .B #imm,Dn — logical immediates, low size bits of
 * Dn modified, upper bits preserved, MOVE-family CCR (N,Z from low size).
 * `size`: 1 = .B, 2 = .W. `kind`: 0 OR, 1 AND, 5 EOR. */
static void emit_immlogic_bw_dn(xt_emit *e, int dn, u32 imm, int size,
                                int kind, regcache *rc) {
    /* For OR/EOR with an imm whose high bits are zero, the operation on a
     * full 32-bit Dn preserves the upper bits naturally. For AND, mask the
     * imm's upper bits to 1 so they're a no-op on Dn. */
    u32 size_mask = (size == 1) ? 0xFFu : 0xFFFFu;
    u32 imm_op = imm & size_mask;
    if (kind == 1) imm_op |= ~size_mask;     /* AND: high bits = 1 */
    emit_load_imm(e, 8, 10, imm_op);
    emit_read_g(e, rc, G_D(dn), 9);
    if      (kind == 0) xt_or (e, 10, 9, 8);
    else if (kind == 1) xt_and(e, 10, 9, 8);
    else                xt_xor(e, 10, 9, 8);
    emit_write_g(e, rc, G_D(dn), 10);
    /* Shift the low-size result to high size so emit_logic_flags' bit-31
     * N and "vreg != 0" Z reflect the size. */
    xt_slli(e, 11, 10, (size == 1) ? 24 : 16);
    emit_logic_flags(e, 11);
    emit_advance(e, 4, 8);
}

/* Number of bytes the helper-path Bcc tail (emit_cond + branchless tail)
 * emits, used to pre-compute the beqz skip distance in fused CMP arms.
 * Must match the actual emit byte-for-byte. */
static u32 fused_helper_bcc_tail_size(int cc) {
    u32 cond_size;
    switch (cc) {
        case 6:  cond_size = 9;  break;  /* NE: extui Z + movi 1 + xor */
        case 7:  cond_size = 6;  break;  /* EQ: extui Z + mov */
        case 13: cond_size = 9;  break;  /* BLT: extui V + extui N + xor */
        case 15: cond_size = 15; break;  /* BLE: extui V + extui Z + extui N + xor + or */
        default: return 0;
    }
    /* emit_bcc_branchless_tail size when g_pc_lit_valid:
     *   movi+sub (2) + l32r (1) + addi+xor+and+xor (4) + s32i (1)
     *   + l32i+addi+addx2+s32i (4) = 12 ops × 3 = 36 bytes.
     * Without literal (fallback): emit_load_imm32 expands the l32r to
     * 10 ops, so 21 ops × 3 = 63 bytes. */
    u32 tail_size = g_pc_lit_valid ? 36 : 63;
    return cond_size + tail_size;
}

/* CMP+Bcc fused condition compute. Given s/d/r registers (shifted-to-high
 * for .W compares, so bit 31 is the sign/result-of-interest), produces
 * `cond` (0 or 1) in a8 without writing R_SR. Saves the ~12 ops of
 * emit_addsub_flags_long_masked's pack/mask + the 3-5 ops of emit_cond's
 * R_SR extraction. Clobbers a8, a11, a12. */
static void emit_cmp_cond_fused(xt_emit *e, int cc, u8 s, u8 d, u8 r) {
    /* For .W CMP, the operands are shifted to bit 31 = high bit, so:
     *   V at bit 31 = (s^d) & (d^r)
     *   N at bit 31 = r itself
     *   Z = (r == 0)
     * Supported:
     *   NE (6)  = !Z  — cond = (r != 0)
     *   EQ (7)  = Z   — cond = (r == 0)
     *   BLT (13) = N^V
     *   BLE (15) = Z | (N^V)
     */
    if (cc == 6) {                     /* NE: r != 0 */
        xt_movi(e, 8, 1);
        xt_bnez(e, r, 6);              /* if r != 0, skip the movi below */
        xt_movi(e, 8, 0);              /* r == 0 → cond = 0 */
    } else if (cc == 7) {              /* EQ: r == 0 */
        xt_movi(e, 8, 0);
        xt_bnez(e, r, 6);              /* if r != 0, skip the movi below */
        xt_movi(e, 8, 1);              /* r == 0 → cond = 1 */
    } else {
        /* Sign-comparison family: needs (s, d, r) and bit-31 analysis. */
        xt_xor (e, 11, s, d);
        xt_xor (e, 12, d, r);
        xt_and (e, 11, 11, 12);        /* bit 31 = V */
        xt_xor (e, 11, 11, r);         /* bit 31 = N^V */
        xt_extui(e, 8, 11, 31, 0);     /* a8 = N^V (0 or 1) */
        if (cc == 15) {                /* BLE — OR in Z */
            xt_bnez(e, r, 6);          /* skip movi if r != 0 */
            xt_movi(e, 8, 1);          /* a8 = 1 (Z is set; cond = 1) */
        }
    }
    sext_memo_invalidate();
}

/* Bcc.S branchless PC + cycle update. Pre-condition: a8 = cond (0 or 1).
 * Writes cpu->pc = (cond ? taken : ft) and cpu->cycles += base_cyc + cond*2.
 * `ft` is the fall-through PC, `disp` is the i8 displacement from the Bcc,
 * `base_cyc` is the not-taken cycle cost folded with any prior absorbed
 * cycles (e.g. a fused CMP's 8 + Bcc's 12 = 20). Clobbers a8..a13. */
static void emit_bcc_branchless_tail(xt_emit *e, u32 ft, i32 disp, i32 base_cyc) {
    xt_movi(e, 10, 0);
    xt_sub (e, 9, 10, 8);                       /* a9 = mask = 0 - cond */
    if (g_pc_lit_valid && ft == g_pc_lit_val) {
        /* 1-op load from the per-block literal — saves 9 ops vs emit_load_imm32. */
        emit_l32r_at(e, 10, g_pc_lit_off, g_pc_lit_entry_off + e->len);
    } else {
        emit_load_imm32(e, 10, 11, ft);         /* fallback: 10-op build */
    }
    if (disp >= -128 && disp <= 127) {
        xt_addi(e, 12, 10, disp);               /* a12 = ft + disp = taken */
    } else {
        u32 taken = ft + (u32)disp;
        emit_load_imm32(e, 12, 11, taken);
    }
    xt_xor (e, 13, 10, 12);
    xt_and (e, 13, 13, 9);
    xt_xor (e, 10, 10, 13);                     /* pc = ft ^ (diff & mask) */
    xt_s32i(e, 10, R_CPU, OFF_PC);
    xt_l32i(e, 11, R_CPU, OFF_CYCLES);
    if (base_cyc >= -128 && base_cyc <= 127) {
        xt_addi(e, 11, 11, base_cyc);
    } else {
        emit_load_imm(e, 10, 12, (u32)base_cyc);
        xt_add(e, 11, 11, 10);
    }
    /* a11 += a8 * 2 — one ADDX2 instead of `slli a8, 8, 1; add a11, 11, 8`. */
    xt_addx2(e, 11, 8, 11);
    xt_s32i(e, 11, R_CPU, OFF_CYCLES);
}

/* Compute the 68000 condition `cc` (2..15) as a 0/1 boolean into a8.
 * Clobbers a9..a12. CCR bits in cpu->sr (cached in R_SR): C=0, V=1, Z=2, N=3. */
static void emit_cond(xt_emit *e, int cc) {
    sext_memo_invalidate();             /* a13 may be written below */
    /* Extract only the CCR bits this condition actually reads. */
    bool need_c = (cc == 2 || cc == 3 || cc == 4 || cc == 5);
    bool need_v = (cc == 8 || cc == 9 || cc == 12 || cc == 13 || cc == 14 || cc == 15);
    bool need_z = (cc == 2 || cc == 3 || cc == 6 || cc == 7 || cc == 14 || cc == 15);
    bool need_n = (cc == 10 || cc == 11 || cc == 12 || cc == 13 || cc == 14 || cc == 15);
    if (need_c) xt_extui(e, 10, R_SR, 0, 0);   /* a10 = C */
    if (need_v) xt_extui(e, 11, R_SR, 1, 0);   /* a11 = V */
    if (need_z) xt_extui(e, 12, R_SR, 2, 0);   /* a12 = Z */
    if (need_n) xt_extui(e, 13, R_SR, 3, 0);   /* a13 = N */
    switch (cc) {
    case 2:  xt_or (e,8,10,12); xt_movi(e,9,1); xt_xor(e,8,8,9); break; /* HI !(C|Z) */
    case 3:  xt_or (e,8,10,12); break;                                 /* LS C|Z   */
    case 4:  xt_movi(e,9,1); xt_xor(e,8,10,9); break;                  /* CC !C    */
    case 5:  xt_mov(e,8,10); break;                                    /* CS C     */
    case 6:  xt_movi(e,9,1); xt_xor(e,8,12,9); break;                  /* NE !Z    */
    case 7:  xt_mov(e,8,12); break;                                    /* EQ Z     */
    case 8:  xt_movi(e,9,1); xt_xor(e,8,11,9); break;                  /* VC !V    */
    case 9:  xt_mov(e,8,11); break;                                    /* VS V     */
    case 10: xt_movi(e,9,1); xt_xor(e,8,13,9); break;                  /* PL !N    */
    case 11: xt_mov(e,8,13); break;                                    /* MI N     */
    case 12: xt_xor(e,8,13,11); xt_movi(e,9,1); xt_xor(e,8,8,9); break;/* GE !(N^V)*/
    case 13: xt_xor(e,8,13,11); break;                                 /* LT N^V   */
    case 14: xt_xor(e,8,13,11); xt_or(e,8,8,12);
             xt_movi(e,9,1); xt_xor(e,8,8,9); break;             /* GT !(Z|(N^V)) */
    case 15: xt_xor(e,8,13,11); xt_or(e,8,8,12); break;          /* LE Z|(N^V)    */
    default: xt_movi(e,8,0); break;
    }
}

/* BRA.S / Bcc.S — a block terminator. Sets cpu->pc to the taken target
 * or the fall-through (branchlessly, via a 0/-1 mask) and adds the
 * taken/not-taken cycle cost. Must match m68k_step's accounting:
 * base = op_pc+2; total cycles 14 taken / 12 not-taken. BSR and the
 * .W/.L forms stay on the helper. */
static void emit_branch(xt_emit *e, u32 op_pc, u16 w) {
    int cc = (w >> 8) & 0xF;
    i32 disp = (i8)(w & 0xFF);
    u32 ft    = op_pc + 2;
    u32 taken = op_pc + 2 + (u32)disp;
    /* Branch sets cpu->pc directly (target overrides any accumulated
     * pc_delta from prior ops in this block); absorb accumulated cycles
     * into the branch's own cycle update. */
    i32 extra_cyc = g_cyc_acc;
    g_pc_acc = 0;
    g_cyc_acc = 0;
    if (cc == 0) {                              /* BRA.S — unconditional */
        if (g_pc_lit_valid && taken == g_pc_lit_val) {
            emit_l32r_at(e, 8, g_pc_lit_off, g_pc_lit_entry_off + e->len);
        } else {
            emit_load_imm32(e, 8, 9, taken);
        }
        xt_s32i(e, 8, R_CPU, OFF_PC);
        xt_l32i(e, 8, R_CPU, OFF_CYCLES);
        i32 total = 14 + extra_cyc;
        if (total >= -128 && total <= 127) {
            xt_addi(e, 8, 8, total);
        } else {
            emit_load_imm(e, 9, 10, (u32)total);
            xt_add(e, 8, 8, 9);
        }
        xt_s32i(e, 8, R_CPU, OFF_CYCLES);
        return;
    }
    emit_cond(e, cc);                           /* a8 = cond (0/1) */
    emit_bcc_branchless_tail(e, ft, disp, 12 + extra_cyc);
    (void)taken;
}

/* ADDQ.W / SUBQ.W #imm,Dn — modify the low 16 bits of Dn, full CCR.
 * `skip_flags` lets the lazy-CC pass drop the (75-byte) flag emission
 * when no consumer reads the flags before the next setter. */
static void emit_addq_w_dn(xt_emit *e, int dn, int imm, bool is_sub, bool skip_flags, regcache *rc) {
    emit_read_g(e, rc, G_D(dn), 11);
    xt_slli(e, 9, 11, 16);
    xt_movi(e, 8, imm);
    xt_slli(e, 8, 8, 16);
    if (is_sub) xt_sub(e, 10, 9, 8);
    else        xt_add(e, 10, 9, 8);
    xt_srli(e, 11, 11, 16);
    xt_slli(e, 11, 11, 16);
    xt_extui(e, 12, 10, 16, 15);
    xt_or  (e, 11, 11, 12);
    emit_write_g(e, rc, G_D(dn), 11);
    if (!skip_flags) emit_addsub_flags_long(e, is_sub, false);
    emit_advance(e, 2, 8);
}

/* LEA <ea>,An — a[an] = effective address. No flags, no memory access.
 * Inlines modes 2 (An), 5 (d16,An), 6 (d8,An,Xn), 7/0 (xxx).W and 7/1
 * (xxx).L. The interpreter charges LEA a flat +4 (total 8) cycles. */
static void emit_lea(xt_emit *e, int an, int srcmode, int srcreg,
                     u16 ext, u32 ext32, regcache *rc) {
    i32 len = 2;
    if (srcmode == 2) {                              /* (An) */
        emit_read_g(e, rc, G_A(srcreg), 8);
    } else if (srcmode == 5) {                       /* (d16,An) */
        emit_read_g(e, rc, G_A(srcreg), 8);
        i16 d16 = (i16)ext;
        if (d16 >= -128 && d16 <= 127) {
            xt_addi(e, 8, 8, d16);                   /* 1-op shortcut */
        } else {
            emit_load_imm(e, 9, 10, (u32)(i32)d16);
            xt_add (e, 8, 8, 9);
        }
        len = 4;
    } else if (srcmode == 6) {                       /* (d8,An,Xn) */
        int ireg = (ext >> 12) & 7;
        emit_read_g(e, rc, G_A(srcreg), 8);
        int g_index = (ext & 0x8000) ? G_A(ireg) : G_D(ireg);
        bool need_sext = !(ext & 0x0800);
        if (need_sext) {
            /* .W index → sext16. Reuse a13 if memoized for this guest reg. */
            if (g_sext_valid && g_sext_src_reg == g_index) {
                xt_add(e, 8, 8, 13);
            } else {
                emit_read_g(e, rc, g_index, 13);
                xt_slli(e, 13, 13, 16);
                xt_srai(e, 13, 13, 16);
                g_sext_src_reg = g_index;
                g_sext_valid = true;
                xt_add(e, 8, 8, 13);
            }
        } else {
            emit_read_g(e, rc, g_index, 9);
            xt_add(e, 8, 8, 9);
        }
        xt_addi(e, 8, 8, (i8)(ext & 0xFF));          /* d8 in -128..127 */
        len = 4;
    } else if (srcmode == 7 && srcreg == 0) {        /* (xxx).W */
        emit_load_imm(e, 8, 9, (u32)(i32)(i16)ext);
        len = 4;
    } else {                                         /* (xxx).L — 7/1 */
        emit_load_imm(e, 8, 9, ext32);
        len = 6;
    }
    emit_write_g(e, rc, G_A(an), 8);
    emit_advance(e, len, 8);
}

/* Conservative CCR classifier for the lazy-CC pass. Returns bit 0 =
 * setter, bit 1 = consumer. Inline-covered opcodes return their exact
 * class. Everything else (the helper fallback path) is conservatively
 * marked SETTER|CONSUMER — m68k_step can do either. */
/* Which CCR bits a Bcc.S condition reads. cc is the 4-bit condition
 * field (bits 11-8). For an unknown / non-Bcc consumer, callers should
 * default to CCR_MASK_ALL. */
static u8 bcc_cc_bits_read(int cc) {
    switch (cc) {
    case 2: case 3:   return CCR_BIT_C | CCR_BIT_Z;             /* HI, LS */
    case 4: case 5:   return CCR_BIT_C;                          /* CC, CS */
    case 6: case 7:   return CCR_BIT_Z;                          /* NE, EQ */
    case 8: case 9:   return CCR_BIT_V;                          /* VC, VS */
    case 10: case 11: return CCR_BIT_N;                          /* PL, MI */
    case 12: case 13: return CCR_BIT_N | CCR_BIT_V;              /* GE, LT */
    case 14: case 15: return CCR_BIT_N | CCR_BIT_V | CCR_BIT_Z;  /* GT, LE */
    default: return CCR_MASK_ALL;
    }
}

static u32 classify_op(u16 w) {
    int top  = (w >> 12) & 0xF;
    int szf  = (w >> 6) & 3;
    int mode = (w >> 3) & 7;
    int op6  = (w >> 6) & 7;
    u32 SET = 1u, CONS = 2u;
    switch (top) {
    case 0x0:                              /* immediate ALU / bit ops */
        return SET;                        /* CMPI/ANDI/ORI/EORI/ADDI/SUBI/BTST set Z */
    case 0x1: case 0x2: case 0x3:          /* MOVE / MOVEA */
        return (op6 == 1) ? 0u : SET;      /* MOVEA → no flags */
    case 0x4: {
        /* LEA: 0100 ddd 111 mmm rrr. No flags. */
        if (op6 == 7) return 0u;
        /* JMP/JSR/RTS/RTE/RTR/NOP/STOP — control flow, treat as consumer-only
         * for our purposes (they keep the CCR you set just before). */
        if (w == 0x4E75 || w == 0x4E71 || w == 0x4E73 || w == 0x4E77 ||
            (w & 0xFFC0) == 0x4EC0 || (w & 0xFFC0) == 0x4E80) {
            return CONS;
        }
        /* SWAP Dn (0x4840-0x4847) / EXT.W Dn (0x4880-0x4887) / EXT.L Dn
         * (0x48C0-0x48C7): set N/Z from result, don't read CCR. */
        if ((w & 0xFFF8) == 0x4840 || (w & 0xFFF8) == 0x4880 ||
            (w & 0xFFF8) == 0x48C0) return SET;
        /* NEG/NOT/CLR/TST .B/.W/.L: set CCR but don't read it (NEGX is
         * the exception — reads X — but that's 0x40xx, excluded here). */
        u32 hi = w & 0xFFC0;
        if (hi == 0x4400 || hi == 0x4440 || hi == 0x4480 ||  /* NEG */
            hi == 0x4600 || hi == 0x4640 || hi == 0x4680 ||  /* NOT */
            hi == 0x4200 || hi == 0x4240 || hi == 0x4280 ||  /* CLR */
            hi == 0x4A00 || hi == 0x4A40 || hi == 0x4A80) {  /* TST */
            return SET;
        }
        /* MOVEM.W/.L (any mode != Dn/An): doesn't touch CCR — transparent
         * in the lazy-CC liveness analysis. Pattern: 0100 1d00 1ss eeeeee
         * where d=dir, ss is constant 01, mode is 2..7 (memory). */
        if ((w & 0xFB80) == 0x4880 && ((w >> 3) & 7) >= 2) {
            return 0u;
        }
        return SET | CONS;
    }
    case 0x5:                              /* ADDQ/SUBQ/Scc/DBcc */
        if (szf == 3) return CONS;         /* Scc/DBcc */
        if (mode == 1) return 0u;          /* to An — no flags */
        return SET;
    case 0x6:                              /* Bcc.S / BRA.S / BSR.S */
        return (((w >> 8) & 0xF) > 1) ? CONS : 0u;
    case 0x7: return SET;                  /* MOVEQ */
    case 0x8: case 0xB: case 0xC: case 0xE: case 0xF:
        return SET;                        /* OR / CMP / AND / shifts / fp */
    case 0x9: case 0xD:                    /* SUB/ADD family — SUBA/ADDA no flags */
        if (szf == 3) return 0u;
        /* ADDX/SUBX (mode 0 with reg-form bit 4 = 0/1): consume X. */
        return SET | CONS;
    case 0xA: return SET | CONS;           /* line-A trap */
    }
    return SET | CONS;
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

    /* 3b. Per-block PC literal. If the terminator is BRA.S or Bcc.S, store
     * its PC constant (taken or ft) in LITERAL_BCC_PC so emit_branch /
     * emit_bcc_branchless_tail can load it with a 1-op `l32r` instead of
     * a 10-op `emit_load_imm32`. */
    g_pc_lit_valid = false;
    g_pc_lit_val = 0;
    g_pc_lit_off = lit_off[LITERAL_BCC_PC];
    g_pc_lit_entry_off = lit_bytes;
    {
        u16 last_op = op_word[n_ops - 1];
        if ((last_op >> 12) == 0x6) {        /* BRA.S / Bcc.S family */
            int cc = (last_op >> 8) & 0xF;
            i32 disp = (i8)(last_op & 0xFF);
            if (disp != 0 && disp != -1) {  /* not .W/.L variants (handled by helper) */
                u32 pc_const;
                if (cc == 0) {
                    /* BRA.S: literal is the taken target. */
                    pc_const = op_pc[n_ops - 1] + 2 + (u32)disp;
                } else {
                    /* Bcc.S: literal is the fall-through PC. */
                    pc_const = op_pc[n_ops - 1] + 2;
                }
                *(u32 *)(base + lit_off[LITERAL_BCC_PC]) = pc_const;
                g_pc_lit_val = pc_const;
                g_pc_lit_valid = true;
            }
        } else if ((last_op >> 12) == 0x5 && ((last_op >> 6) & 3) == 3
                   && ((last_op >> 3) & 7) == 1) {
            /* DBcc Dn, disp16 — fall-through PC is op_pc + 4 (skips the
             * disp16 word). The inlined DBcc tail derives the taken target
             * via xt_addi from this literal. */
            u32 pc_const = op_pc[n_ops - 1] + 4;
            *(u32 *)(base + lit_off[LITERAL_BCC_PC]) = pc_const;
            g_pc_lit_val = pc_const;
            g_pc_lit_valid = true;
        }
    }

    xt_emit e;
    xt_init(&e, base + entry_off, total - entry_off);

    /* 5. Lazy-CCR analysis. Backward scan. For each op, the setter side
     * happens "after" the consumer side in execution order — but in a
     * backward scan that means we observe the setter FIRST (downstream)
     * and the consumer SECOND (upstream). So: setter first (decides
     * whether *its own* flag emission is dead based on downstream
     * `need`), then consumer (sets `need` so the next upstream setter
     * knows its flags are read by this op). */
    bool flags_dead[M68K_MAX_OPS_PER_BLOCK] = {0};
    u8 flags_needed[M68K_MAX_OPS_PER_BLOCK];
    for (u32 k = 0; k < M68K_MAX_OPS_PER_BLOCK; k++) flags_needed[k] = CCR_MASK_ALL;
    {
        /* Cross-block lazy-CC: if the block ends with a control-flow op
         * (BRA.S unconditional or a fall-through ender) and the next
         * block's first op is a SETTER-without-CONSUMER class, the
         * current block's last setter's flags are dead. Bench's hot
         * block at 0x03DF40 falls through to 0x03DF58 which starts with
         * MOVE.W D5,D4 (a setter) — without this, the MOVE.W (d8,An,Xn)
         * deep inside 0x03DF40 emits a full flag computation every
         * iteration even though nothing observable reads it. */
        bool need_initial = true;
        u16 last_op = op_word[n_ops - 1];
        if (n_ops >= 1) {
            int t = (last_op >> 12) & 0xF;
            int cc = (last_op >> 8) & 0xF;
            /* Helper: walk forward from `pc`, skipping CCR-neutral ops
             * (classify_op returns 0). Returns true if we find a
             * SET-without-CONS op before any CONS or unknown op. That
             * means CCR set before this point is dead — overwritten
             * without ever being read. */
            #define PC_OVERWRITES_CCR(pc_) ({ \
                u32 _pc = (pc_); \
                bool _r = false; \
                for (int _k = 0; _k < 8; _k++) { \
                    m68k_decoded _nd = m68k_decode_at(cpu, _pc); \
                    u32 _c = classify_op(_nd.opcode); \
                    if (_c == 0) { _pc += _nd.length; continue; } \
                    _r = ((_c & 1u) && !(_c & 2u)); \
                    break; \
                } \
                _r; })
            if (t == 0x6 && cc == 0) {
                /* BRA.S — single known target. */
                i32 disp = (i8)(last_op & 0xFF);
                u32 next_pc = op_pc[n_ops - 1] + 2 + (u32)disp;
                if (PC_OVERWRITES_CCR(next_pc)) need_initial = false;
            } else if (t == 0x6 && cc >= 2) {
                /* Bcc.S — two targets (taken and fall-through). CCR set
                 * by upstream setter is dead AFTER the Bcc iff BOTH
                 * destinations overwrite CCR with their first op. The
                 * Bcc itself still reads CCR — that's handled by the
                 * within-block scan when Bcc is marked CONS. */
                i32 disp = (i8)(last_op & 0xFF);
                u32 taken_pc = op_pc[n_ops - 1] + 2 + (u32)disp;
                u32 ft_pc    = op_pc[n_ops - 1] + 2;
                if (PC_OVERWRITES_CCR(taken_pc) && PC_OVERWRITES_CCR(ft_pc))
                    need_initial = false;
            } else if (t != 0x6 && (last_op != 0x4E75) && (last_op != 0x4E73)
                       && (last_op != 0x4E77) && ((last_op & 0xFFC0) != 0x4EC0)
                       && ((last_op & 0xFFC0) != 0x4E80)
                       && (last_op != 0x4E72)) {
                /* Fall-through: not Bcc/BSR, not RTS/RTE/RTR/JMP/JSR/STOP. */
                if (PC_OVERWRITES_CCR(cur)) need_initial = false;
            }
            #undef PC_OVERWRITES_CCR
        }
        bool need = need_initial;
        u8 need_bits = need_initial ? CCR_MASK_ALL : 0;
        for (int i = (int)n_ops - 1; i >= 0; i--) {
            u16 w = op_word[i];
            u32 cls = classify_op(w);
            bool is_setter   = (cls & 1u) != 0;
            bool is_consumer = (cls & 2u) != 0;
            if (is_setter) {
                flags_dead[i] = !need;
                flags_needed[i] = need_bits;
                need = false;
                need_bits = 0;
            }
            if (is_consumer) {
                need = true;
                int t = (w >> 12) & 0xF;
                int cc = (w >> 8) & 0xF;
                if (t == 0x6 && cc >= 2) {
                    /* Bcc.S — only the bits this condition reads. */
                    need_bits |= bcc_cc_bits_read(cc);
                } else {
                    /* Unknown consumer (RTS/JMP/helper): conservatively all. */
                    need_bits = CCR_MASK_ALL;
                }
            }
        }
        /* Expanded lazy-CC eligibility (M6.6): allow flag-skip for any op
         * whose inline emitter respects the `skip_flags` parameter and
         * whose `classify_op` SET/CONS classification is accurate. The
         * backward scan already only marks an op dead when no consumer
         * reads its flags before the next setter — the only risk is
         * misclassification. Restrict to ops we know are inlined with
         * skip_flags support and have accurate CCR class:
         *   - ADDQ.W/SUBQ.W to Dn (top=5)
         *   - MOVE.W to Dn        (top=3, op6=0)
         *   - ADD.L/SUB.L Dm,Dn   (top=8,9,B,C,D with mode=0)
         *   - AND.L/EOR.L Dm,Dn   (top=B,C with mode=0)
         *   - ORI/ANDI/ADDI/SUBI/EORI.L #imm32,Dn (top=0 with select rr)
         *   - MOVEQ                (top=7)
         */
        for (u32 i = 0; i < n_ops; i++) {
            u16 w = op_word[i];
            int top = (w >> 12) & 0xF;
            int sf  = (w >> 6) & 3;
            int mm  = (w >> 3) & 7;
            int op6 = (w >> 6) & 7;
            int rr  = (w >> 9) & 7;
            bool b1 = false;
            b1 |= (top == 0x5 && sf == 1 && mm == 0);              /* ADDQ.W Dn */
            b1 |= (top == 0x3 && op6 == 0);                        /* MOVE.W Dn */
            b1 |= (top == 0x3 && op6 == 2);                        /* MOVE.W (As)→(Ad), Dn,(An) */
            b1 |= (top == 0x2 && op6 == 3);                        /* MOVE.L Dn,(An)+ */
            b1 |= (top == 0x2 && op6 == 0);                        /* MOVE.L <ea>,Dn */
            b1 |= (top == 0xD && sf == 2 && !((w >> 8) & 1) && mm == 0);  /* ADD.L Dm,Dn */
            b1 |= (top == 0x9 && sf == 2 && !((w >> 8) & 1) && mm == 0);  /* SUB.L Dm,Dn */
            b1 |= (top == 0xC && sf == 2 && !((w >> 8) & 1) && mm == 0);  /* AND.L Dm,Dn */
            b1 |= (top == 0x8 && sf == 2 && !((w >> 8) & 1) && mm == 0);  /* OR.L Dm,Dn */
            b1 |= ((top == 0xD || top == 0x9) && sf == 1 && !((w >> 8) & 1) && mm == 0); /* ADD.W/SUB.W Dm,Dn */
            b1 |= (top == 0x4 && ((w >> 8) & 0xF) == 0xA && sf == 2 && mm == 0); /* TST.L Dn */
            b1 |= (top == 0xB && sf == 2 && ((w >> 8) & 1) && mm == 0);   /* EOR.L Dn,Dm */
            b1 |= (top == 0x0 && !((w >> 8) & 1) && sf == 2 && mm == 0
                   && rr != 4 && rr != 7);                                 /* ORI/ANDI/ADDI/SUBI/EORI.L #imm32,Dn */
            b1 |= (top == 0x7);                                            /* MOVEQ */
            if (!b1) flags_dead[i] = false;
        }
    }

    /* 5b. Register-cache analysis. Count how often each guest D/A register
     * appears in this block (as source or destination), pick up to 4 of
     * the most-used to live in a4..a7 across the block. Skip caching when
     * the block is helper-heavy (every helper costs a flush + reload, and
     * those add up faster than the per-op savings). */
    regcache rc;
    memset(&rc, 0, sizeof(rc));
    for (int i = 0; i < 16; i++) rc.xt[i] = -1;
    {
        u32 use[16] = {0};
        u32 inline_count = 0, helper_count = 0;
        for (u32 i = 0; i < n_ops; i++) {
            u16 w = op_word[i];
            int top = (w >> 12) & 0xF;
            int dr  = (w >> 9) & 7;
            int dm  = (w >> 6) & 7;
            int sm  = (w >> 3) & 7;
            int sr  = w & 7;
            /* Conservative guest-reg sniffing: count every (mode, reg)
             * occurrence as one use. Modes 0/1 → D/A; modes 2..6 → A. */
            int g_dst = -1, g_src = -1;
            if (dm == 0)      g_dst = G_D(dr);
            else if (dm == 1) g_dst = G_A(dr);
            else if (dm <= 6) g_dst = G_A(dr);  /* memory dst uses An */
            if (sm == 0)      g_src = G_D(sr);
            else if (sm == 1) g_src = G_A(sr);
            else if (sm <= 6) g_src = G_A(sr);
            if (g_dst >= 0) use[g_dst]++;
            if (g_src >= 0 && g_src != g_dst) use[g_src]++;
            /* Bcc/BRA/MOVEQ/CMP all touch D/A too — but the per-arm
             * decoding above is rough; under-counting here only means
             * "this register won't be cached", which is safe. */
            u32 cls = classify_op(w);
            bool inline_likely = (cls != (1u | 2u));  /* SET|CONS is helper-conservative */
            (void)top; (void)inline_likely;
        }
        /* Pick the top-4 most-used. */
        for (int slot = 0; slot < 4; slot++) {
            int best = -1; u32 best_n = 0;
            for (int g = 0; g < 16; g++) {
                if (rc.xt[g] >= 0) continue;
                if (use[g] > best_n) { best = g; best_n = use[g]; }
            }
            if (best < 0 || best_n < 2) break;
            rc.xt[best] = (i8)cache_xt_slot(slot);
            rc.guest[slot] = (u8)best;
            rc.active++;
        }
        (void)inline_count; (void)helper_count;
    }

    /* 5d. Determine whether the prologue's R_SR reload is needed. Skip
     * when no inline op reads or writes R_SR — for fused-CMP+Bcc-
     * terminated blocks where every other op has flags_dead, the
     * prologue load is dead (helper-bridge fallbacks reload R_SR on
     * their own). Saves 1 op per execution of such blocks. */
    bool block_needs_sr_load = false;
    bool terminator_fused = false;
    if (n_ops >= 2) {
        u16 last = op_word[n_ops - 1];
        u16 prev = op_word[n_ops - 2];
        if ((last >> 12) == 0x6) {
            int cc = (last >> 8) & 0xF;
            i32 disp = (i8)(last & 0xFF);
            if ((cc == 6 || cc == 7 || cc == 13 || cc == 15) && disp != 0 && disp != -1) {
                int ptop = (prev >> 12) & 0xF;
                int pmode = (prev >> 3) & 7;
                int pszf = (prev >> 6) & 3;
                int pbit8 = (prev >> 8) & 1;
                if (ptop == 0xB && pszf == 1 && pbit8 == 0 && (pmode == 2 || pmode == 5)) {
                    terminator_fused = true;
                }
            }
        }
    }
    for (u32 k = 0; k < n_ops; k++) {
        /* Skip the fused CMP and Bcc — their fast path does no R_SR
         * access, and the helper-path bridge reloads R_SR before
         * touching it. */
        if (terminator_fused && k >= n_ops - 2) continue;
        u16 w = op_word[k];
        u32 cls = classify_op(w);
        /* SET op writes R_SR via read-modify-write unless flags are dead. */
        if ((cls & 1u) && !flags_dead[k]) {
            block_needs_sr_load = true; break;
        }
        /* CONS op reads R_SR — but DBcc with cc∈{T,F} doesn't actually
         * test CCR, only the inlined DBT/DBF paths run, so don't require
         * the prologue load in that case. */
        if (cls & 2u) {
            bool is_dbcc = ((w >> 12) == 0x5) && (((w >> 6) & 3) == 3)
                           && (((w >> 3) & 7) == 1);
            int cc = (w >> 8) & 0xF;
            if (is_dbcc && cc < 2) {
                /* DBT/DBF — no R_SR read. */
            } else {
                block_needs_sr_load = true; break;
            }
        }
        /* Bcc.S (cc != 0 BRA, != 1 BSR) reads R_SR via emit_cond. */
        if ((w >> 12) == 0x6) {
            int cc = (w >> 8) & 0xF;
            if (cc >= 2) { block_needs_sr_load = true; break; }
        }
    }

    /* 4. Prologue: a3 = cpu_state base; stash the CALL0 return PC;
     * load R_SR (when block needs it); load every D/A cache slot. */
    g_sr_dirty = false;
    g_pc_acc = 0;
    g_cyc_acc = 0;
    g_sext_valid = false;
    g_sext_src_reg = -1;
    emit_l32r_at(&e, R_CPU, lit_off[ADDR_CPU_BASE], entry_off + e.len);
    xt_s32i(&e, 0, R_CPU, OFF_JITRETPC);
    if (block_needs_sr_load) {
        emit_sr_reload(&e);
    }

    /* 5c. Emit the cache-load epilogue-of-prologue: one l32i per cached
     * slot, run once before the body starts. */
    for (int slot = 0; slot < rc.active; slot++) {
        int gi = rc.guest[slot];
        if (gi < 8) xt_l32i(&e, cache_xt_slot(slot), R_CPU, OFF_D(gi));
        else        xt_l32i(&e, cache_xt_slot(slot), R_CPU, OFF_A(gi - 8));
    }

    /* 6. Body. */
    u32 inline_ops = 0, helper_ops = 0;
    for (u32 i = 0; i < n_ops; i++) {
        u16 w = op_word[i];
        int top = (w >> 12) & 0xF;
        bool done = false;

        int szf  = (w >> 6) & 3;
        int mode = (w >> 3) & 7;

        if (top == 0x7) {                  /* MOVEQ */
            emit_moveq(&e, w, flags_dead[i], &rc);
            inline_ops++; done = true;
        } else if (w == 0x4E71) {          /* NOP */
            emit_advance(&e, 2, 4);
            inline_ops++; done = true;
        } else if (top == 0xD && szf == 2 && !((w >> 8) & 1) && mode == 0) {
            /* ADD.L Dm,Dn */
            emit_add_l_dd(&e, (w >> 9) & 7, w & 7, flags_dead[i], &rc);
            inline_ops++; done = true;
        } else if (top == 0x5 && szf == 2 && mode == 0) {
            /* ADDQ.L / SUBQ.L #imm,Dn */
            int data = (w >> 9) & 7; if (data == 0) data = 8;
            emit_addq_l_dd(&e, w & 7, data, (w >> 8) & 1, flags_dead[i], &rc);
            inline_ops++; done = true;
        } else if (top == 0x5 && szf != 3 && mode == 1) {
            /* ADDQ / SUBQ #imm,An */
            int data = (w >> 9) & 7; if (data == 0) data = 8;
            emit_addq_an(&e, w & 7, ((w >> 8) & 1) ? -data : data, &rc);
            inline_ops++; done = true;
        } else if (top == 0x2 && ((w >> 3) & 7) == 7 && (w & 7) == 4) {
            /* MOVE.L / MOVEA.L #imm32,<dst>  (src = immediate). */
            u32 imm = mac_read32(cpu->mem, op_pc[i] + 2);
            int dst_mode = (w >> 6) & 7, dst_reg = (w >> 9) & 7;
            if (dst_mode == 0) {
                emit_move_l_imm_dn(&e, dst_reg, imm, flags_dead[i], &rc);
                inline_ops++; done = true;
            } else if (dst_mode == 1) {
                emit_movea_l_imm(&e, dst_reg, imm, &rc);
                inline_ops++; done = true;
            }
        } else if (top == 0x2 && mode == 0 && ((w >> 6) & 7) == 0) {
            /* MOVE.L Dm,Dn */
            emit_move_l_dd(&e, (w >> 9) & 7, w & 7, flags_dead[i], &rc);
            inline_ops++; done = true;
        } else if (top == 0x2 && ((w >> 6) & 7) == 1 && mode <= 1) {
            emit_movea_l_reg(&e, (w >> 9) & 7, w & 7, mode == 1, &rc);
            inline_ops++; done = true;
        } else if (top == 0xD && szf == 3 && !((w >> 8) & 1) && mode <= 1) {
            /* ADDA.W <Dm|Am>,An */
            emit_adda_w_reg(&e, (w >> 9) & 7, w & 7, mode == 1, &rc);
            inline_ops++; done = true;
        } else if ((top == 0xD || top == 0x9) && szf == 3 && !((w >> 8) & 1)
                   && mode == 7 && (w & 7) == 4) {
            /* ADDA.W / SUBA.W #imm16,An — boot-hot 0xD0FC.  mode 7/4 = #imm. */
            int an = (w >> 9) & 7;
            u16 imm = mac_read16(cpu->mem, op_pc[i] + 2);
            emit_adda_w_imm(&e, an, (i16)imm, top == 0x9, &rc);
            inline_ops++; done = true;
        } else if (top == 0x5 && szf == 1 && mode == 0) {
            /* ADDQ.W / SUBQ.W #imm,Dn */
            int data = (w >> 9) & 7; if (data == 0) data = 8;
            emit_addq_w_dn(&e, w & 7, data, (w >> 8) & 1, flags_dead[i], &rc);
            inline_ops++; done = true;
        } else if (top == 0x3 && ((w >> 6) & 7) == 0 && mode == 0) {
            /* MOVE.W Dm,Dn — aggressive (held back under --diff-jit). */
            int dn = (w >> 9) & 7;
            int dm = w & 7;
            emit_read_g(&e, &rc, G_D(dm), 9);
            emit_read_g(&e, &rc, G_D(dn), 11);
            xt_srli (&e, 11, 11, 16);
            xt_slli (&e, 11, 11, 16);
            xt_extui(&e, 12, 9, 0, 15);
            xt_or   (&e, 11, 11, 12);
            emit_write_g(&e, &rc, G_D(dn), 11);
            if (!flags_dead[i]) {
                xt_slli (&e, 8, 9, 16);
                emit_logic_flags(&e, 8);
            }
            emit_advance(&e, 2, 8);
            inline_ops++; done = true;
        } else if (top == 0x3 && ((w >> 6) & 7) == 2 && mode == 2) {
            /* MOVE.W (As),(Ad) — bench-hot 0x3692 (~3.5 %).
             * Combined bounds: (src|dst) & RAM_BOUNDS == 0 iff both
             * addresses are in RAM and aligned. */
            int src_an = w & 7;
            int dst_an = (w >> 9) & 7;

            emit_advance_flush(&e);                  /* before An reads */
            emit_read_g(&e, &rc, G_A(src_an), 8);    /* src addr */
            emit_read_g(&e, &rc, G_A(dst_an), 9);    /* dst addr */
            xt_or   (&e, 10, 8, 9);
            emit_l32r_at(&e, 11, lit_off[LITERAL_RAM_BOUNDS],
                         entry_off + e.len);
            xt_and  (&e, 10, 10, 11);
            emit_cache_flush(&e, &rc);   /* before conditional helper */
            i32 op_pc_delta_mm = 2, op_cyc_mm = 8;
            xt_beqz (&e, 10, (i32)(6u + helper_step_after_flush_undo_size(&rc, op_pc_delta_mm, op_cyc_mm)));

            /* Helper. */
            emit_helper_step_after_flush_undo(&e, lit_off[HELPER_M68K_STEP],
                                              entry_off, &rc, op_pc_delta_mm, op_cyc_mm);
            u32 jmm_pos = e.len;
            xt_j    (&e, 4);

            /* Fast path. */
            emit_l32r_at(&e, 11, lit_off[ADDR_RAM_BASE],
                         entry_off + e.len);
            xt_add  (&e, 8, 8, 11);                  /* a8 = ram + src */
            xt_add  (&e, 9, 9, 11);                  /* a9 = ram + dst */
            xt_l8ui (&e, 10, 8, 0);                  /* read high */
            xt_l8ui (&e, 12, 8, 1);                  /* read low  */
            xt_s8i  (&e, 10, 9, 0);                  /* write high */
            xt_s8i  (&e, 12, 9, 1);                  /* write low  */
            if (!flags_dead[i]) {
                /* Assemble .W only when flags are actually consumed. */
                xt_slli (&e, 10, 10, 8);
                xt_or   (&e, 10, 10, 12);            /* a10 = .W value */
                xt_slli (&e, 8, 10, 16);
                emit_logic_flags(&e, 8);
            }
            emit_advance(&e, op_pc_delta_mm, op_cyc_mm);

            u32 mm_here = e.len;
            i32 mm_off = (i32)(mm_here - jmm_pos) - 4;
            u32 mmw = ((u32)((u32)mm_off & 0x3FFFFu) << 6) | 0x06u;
            base[entry_off + jmm_pos    ] = (u8)mmw;
            base[entry_off + jmm_pos + 1] = (u8)(mmw >> 8);
            base[entry_off + jmm_pos + 2] = (u8)(mmw >> 16);

            inline_ops++; done = true;
        } else if (top == 0x3 && ((w >> 6) & 7) == 0 && mode == 3) {
            /* MOVE.W (An)+,Dn — boot-hot 0x3018 (~65 K). Same shape as
             * the .W (An),Dn arm but with An post-increment by 2. */
            int dn = (w >> 9) & 7;        /* dst Dn */
            int an = w & 7;               /* src An (post-incr) */

            emit_advance_flush(&e);
            emit_read_g(&e, &rc, G_A(an), 8);
            emit_l32r_at(&e, 9, lit_off[LITERAL_RAM_BOUNDS],
                         entry_off + e.len);
            xt_and  (&e, 10, 8, 9);
            emit_cache_flush(&e, &rc);
            i32 op_pc_wp = 2, op_cyc_wp = 8;
            /* Custom MMIO helper bridge — replaces m68k_step for VIA reads. */
            u32 wp_bridge_size = emit_jit_fast_helper_size(&rc);
            xt_beqz (&e, 10, (i32)(6u + wp_bridge_size));
            /* a8 still has An (the addr). Use it for jit_arg1 (helper ignores
             * it and uses jit_arg2's packed dn|an<<4 instead). */
            emit_jit_fast_helper(&e, 8, dn | (an << 4),
                                 lit_off[HELPER_JIT_MOVE_W_POSTINC_TO_DN],
                                 entry_off, &rc);
            (void)op_pc_wp; (void)op_cyc_wp;
            u32 jwp_pos = e.len;
            xt_j    (&e, 4);
            /* Fast path: read 2 BE bytes into a10 (.W value). */
            emit_l32r_at(&e, 9, lit_off[ADDR_RAM_BASE],
                         entry_off + e.len);
            xt_add  (&e, 9, 9, 8);
            xt_l8ui (&e, 10, 9, 0);
            xt_l8ui (&e, 11, 9, 1);
            xt_slli (&e, 10, 10, 8);
            xt_or   (&e, 10, 10, 11);                    /* a10 = .W */
            {
                int dn_xt = cache_lookup(&rc, G_D(dn));
                u8 dn_reg = (dn_xt >= 0) ? (u8)dn_xt : 11;
                if (dn_xt < 0) {
                    emit_read_g(&e, &rc, G_D(dn), 11);
                }
                xt_srli(&e, dn_reg, dn_reg, 16);
                xt_slli(&e, dn_reg, dn_reg, 16);
                xt_or  (&e, dn_reg, dn_reg, 10);
                if (dn_xt >= 0) {
                    for (int s = 0; s < rc.active; s++)
                        if (rc.guest[s] == (u8)G_D(dn)) { rc.dirty |= (u16)(1u << s); break; }
                } else {
                    emit_write_g(&e, &rc, G_D(dn), 11);
                }
            }
            /* Post-increment An by 2. */
            xt_addi (&e, 8, 8, 2);
            emit_write_g(&e, &rc, G_A(an), 8);
            if (!flags_dead[i]) {
                xt_slli(&e, 8, 10, 16);
                emit_logic_flags(&e, 8);
            }
            emit_advance(&e, op_pc_wp, op_cyc_wp);

            u32 here_wp = e.len;
            i32 jo_wp = (i32)(here_wp - jwp_pos) - 4;
            u32 jwwp = ((u32)((u32)jo_wp & 0x3FFFFu) << 6) | 0x06u;
            base[entry_off + jwp_pos    ] = (u8)jwwp;
            base[entry_off + jwp_pos + 1] = (u8)(jwwp >> 8);
            base[entry_off + jwp_pos + 2] = (u8)(jwwp >> 16);

            inline_ops++; done = true;
        } else if (top == 0x2 && ((w >> 6) & 7) == 0 && mode == 3) {
            /* MOVE.L (An)+,Dn — common pair to the 0x20C1 store pattern.
             * Bounds-check the An address; on fast path read 4 BE bytes
             * into Dn, post-increment An by 4, emit logic flags. */
            int dn = (w >> 9) & 7;        /* dst Dn */
            int an = w & 7;               /* src An (post-incr) */

            emit_advance_flush(&e);
            emit_read_g(&e, &rc, G_A(an), 8);            /* a8 = An */
            emit_l32r_at(&e, 9, lit_off[LITERAL_RAM_BOUNDS],
                         entry_off + e.len);
            xt_and  (&e, 10, 8, 9);
            emit_cache_flush(&e, &rc);
            i32 op_pc_lL = 2, op_cyc_lL = 8;
            xt_beqz (&e, 10, (i32)(6u + helper_step_after_flush_undo_size(&rc, op_pc_lL, op_cyc_lL)));
            emit_helper_step_after_flush_undo(&e, lit_off[HELPER_M68K_STEP],
                                              entry_off, &rc, op_pc_lL, op_cyc_lL);
            u32 jlL_pos = e.len;
            xt_j    (&e, 4);
            /* Fast path: read 4 BE bytes into a10 (the .L value). */
            emit_l32r_at(&e, 9, lit_off[ADDR_RAM_BASE],
                         entry_off + e.len);
            xt_add  (&e, 9, 9, 8);                       /* a9 = ram + An */
            xt_l8ui (&e, 11, 9, 0);                      /* high byte */
            xt_l8ui (&e, 12, 9, 1);
            xt_slli (&e, 10, 11, 24);
            xt_slli (&e, 12, 12, 16);
            xt_or   (&e, 10, 10, 12);
            xt_l8ui (&e, 11, 9, 2);
            xt_l8ui (&e, 12, 9, 3);
            xt_slli (&e, 11, 11, 8);
            xt_or   (&e, 10, 10, 11);
            xt_or   (&e, 10, 10, 12);                    /* a10 = .L value */
            emit_write_g(&e, &rc, G_D(dn), 10);
            /* Post-increment An by 4. */
            xt_addi (&e, 8, 8, 4);
            emit_write_g(&e, &rc, G_A(an), 8);
            if (!flags_dead[i]) emit_logic_flags(&e, 10);
            emit_advance(&e, op_pc_lL, op_cyc_lL);

            u32 here_lL = e.len;
            i32 jo_lL = (i32)(here_lL - jlL_pos) - 4;
            u32 jwL = ((u32)((u32)jo_lL & 0x3FFFFu) << 6) | 0x06u;
            base[entry_off + jlL_pos    ] = (u8)jwL;
            base[entry_off + jlL_pos + 1] = (u8)(jwL >> 8);
            base[entry_off + jlL_pos + 2] = (u8)(jwL >> 16);

            inline_ops++; done = true;
        } else if (top == 0x2 && ((w >> 6) & 7) == 4 && mode == 0) {
            /* MOVE.L Dn,-(An) — pre-decrement push pattern (boot 0x24C3
             * ~5 K, bench 0x2F08 ~2 K). Same shape as MOVE.L Dn,(An)+
             * but with An decremented by 4 before the write. */
            int an = (w >> 9) & 7;
            int dm = w & 7;

            emit_advance_flush(&e);
            emit_read_g(&e, &rc, G_A(an), 8);
            xt_addi(&e, 8, 8, -4);
            emit_l32r_at(&e, 9, lit_off[LITERAL_RAM_BOUNDS],
                         entry_off + e.len);
            xt_and  (&e, 10, 8, 9);
            emit_cache_flush(&e, &rc);
            i32 op_pc_pdl = 2, op_cyc_pdl = 8;
            xt_beqz (&e, 10, (i32)(6u + helper_step_after_flush_undo_size(&rc, op_pc_pdl, op_cyc_pdl)));
            emit_helper_step_after_flush_undo(&e, lit_off[HELPER_M68K_STEP],
                                              entry_off, &rc, op_pc_pdl, op_cyc_pdl);
            u32 jpdl_pos = e.len;
            xt_j    (&e, 4);
            emit_read_g(&e, &rc, G_D(dm), 10);
            emit_l32r_at(&e, 9, lit_off[ADDR_RAM_BASE],
                         entry_off + e.len);
            xt_add  (&e, 9, 9, 8);
            xt_extui(&e, 11, 10, 24, 7); xt_s8i(&e, 11, 9, 0);
            xt_extui(&e, 11, 10, 16, 7); xt_s8i(&e, 11, 9, 1);
            xt_extui(&e, 11, 10,  8, 7); xt_s8i(&e, 11, 9, 2);
            xt_extui(&e, 11, 10,  0, 7); xt_s8i(&e, 11, 9, 3);
            emit_write_g(&e, &rc, G_A(an), 8);
            if (!flags_dead[i]) emit_logic_flags(&e, 10);
            emit_advance(&e, op_pc_pdl, op_cyc_pdl);

            u32 here = e.len;
            i32 jo = (i32)(here - jpdl_pos) - 4;
            u32 jw = ((u32)((u32)jo & 0x3FFFFu) << 6) | 0x06u;
            base[entry_off + jpdl_pos    ] = (u8)jw;
            base[entry_off + jpdl_pos + 1] = (u8)(jw >> 8);
            base[entry_off + jpdl_pos + 2] = (u8)(jw >> 16);

            inline_ops++; done = true;
        } else if (top == 0x2 && ((w >> 6) & 7) == 3 && mode == 0) {
            /* MOVE.L Dn,(An)+ — boot-hot 0x20C1 at 262 K execs / 60 M cycles.
             * Bounds check the An address; on fast path do 4 byte writes
             * (BE), post-increment An by 4, emit logic flags if needed. */
            int an = (w >> 9) & 7;        /* dst An (post-incr) */
            int dm = w & 7;               /* src Dn */

            emit_advance_flush(&e);
            emit_read_g(&e, &rc, G_A(an), 8);            /* a8 = An */
            emit_l32r_at(&e, 9, lit_off[LITERAL_RAM_BOUNDS],
                         entry_off + e.len);
            xt_and  (&e, 10, 8, 9);
            emit_cache_flush(&e, &rc);
            i32 op_pc_lp = 2, op_cyc_lp = 8;
            xt_beqz (&e, 10, (i32)(6u + helper_step_after_flush_undo_size(&rc, op_pc_lp, op_cyc_lp)));
            emit_helper_step_after_flush_undo(&e, lit_off[HELPER_M68K_STEP],
                                              entry_off, &rc, op_pc_lp, op_cyc_lp);
            u32 jlp_pos = e.len;
            xt_j    (&e, 4);
            /* Fast path: load Dn, write 4 BE bytes, post-incr An. */
            emit_read_g(&e, &rc, G_D(dm), 10);           /* a10 = Dn */
            emit_l32r_at(&e, 9, lit_off[ADDR_RAM_BASE],
                         entry_off + e.len);
            xt_add  (&e, 9, 9, 8);                       /* a9 = ram + An */
            xt_extui(&e, 11, 10, 24, 7); xt_s8i(&e, 11, 9, 0);
            xt_extui(&e, 11, 10, 16, 7); xt_s8i(&e, 11, 9, 1);
            xt_extui(&e, 11, 10,  8, 7); xt_s8i(&e, 11, 9, 2);
            xt_extui(&e, 11, 10,  0, 7); xt_s8i(&e, 11, 9, 3);
            /* Post-increment An by 4. a8 still holds pre-incr An. */
            xt_addi (&e, 8, 8, 4);
            emit_write_g(&e, &rc, G_A(an), 8);
            if (!flags_dead[i]) emit_logic_flags(&e, 10);
            emit_advance(&e, op_pc_lp, op_cyc_lp);

            u32 here = e.len;
            i32 jo = (i32)(here - jlp_pos) - 4;
            u32 jww = ((u32)((u32)jo & 0x3FFFFu) << 6) | 0x06u;
            base[entry_off + jlp_pos    ] = (u8)jww;
            base[entry_off + jlp_pos + 1] = (u8)(jww >> 8);
            base[entry_off + jlp_pos + 2] = (u8)(jww >> 16);

            inline_ops++; done = true;
        } else if (top == 0x3 && ((w >> 6) & 7) == 2 && mode == 0) {
            /* MOVE.W Dn,(An) — bench-hot 0x3484 (~3.5 %).
             *
             * Writes bypass mac_write_watch's SMC tracker — but a guest
             * write of a 16-bit value to RAM rarely overlaps the JIT's
             * compiled code pages (code pages are usually segment loads
             * via MOVE.L). If a future workload tickles this, add a
             * stripped watch helper. */
            int an = (w >> 9) & 7;        /* dst An */
            int dn = w & 7;               /* src Dn */

            emit_advance_flush(&e);                  /* before An read */
            emit_read_g(&e, &rc, G_A(an), 8);
            emit_l32r_at(&e, 9, lit_off[LITERAL_RAM_BOUNDS],
                         entry_off + e.len);
            xt_and  (&e, 10, 8, 9);
            emit_cache_flush(&e, &rc);   /* before conditional helper */
            i32 op_pc_dnan = 2, op_cyc_dnan = 8;
            xt_beqz (&e, 10, (i32)(6u + helper_step_after_flush_undo_size(&rc, op_pc_dnan, op_cyc_dnan)));

            /* Helper. */
            emit_helper_step_after_flush_undo(&e, lit_off[HELPER_M68K_STEP],
                                              entry_off, &rc, op_pc_dnan, op_cyc_dnan);

            /* j over fast — backpatch. */
            u32 jw_pos = e.len;
            xt_j    (&e, 4);

            /* Fast path. */
            emit_read_g(&e, &rc, G_D(dn), 10);       /* a10 = Dn */
            xt_extui(&e, 11, 10, 8, 7);              /* a11 = .W high byte */
            xt_extui(&e, 12, 10, 0, 7);              /* a12 = .W low byte  */
            emit_l32r_at(&e, 9, lit_off[ADDR_RAM_BASE],
                         entry_off + e.len);
            xt_add  (&e, 9, 9, 8);
            xt_s8i  (&e, 11, 9, 0);
            xt_s8i  (&e, 12, 9, 1);
            if (!flags_dead[i]) {
                xt_slli (&e, 8, 10, 16);
                emit_logic_flags(&e, 8);
            }
            emit_advance(&e, op_pc_dnan, op_cyc_dnan);

            u32 here = e.len;
            i32 jo = (i32)(here - jw_pos) - 4;
            u32 jww = ((u32)((u32)jo & 0x3FFFFu) << 6) | 0x06u;
            base[entry_off + jw_pos    ] = (u8)jww;
            base[entry_off + jw_pos + 1] = (u8)(jww >> 8);
            base[entry_off + jw_pos + 2] = (u8)(jww >> 16);

            inline_ops++; done = true;
        } else if (top == 0xB && szf == 3 && ((w >> 8) & 1) == 0 && mode == 5) {
            /* CMPA.W (d16,An),An — bench-hot 0xBC6D (~6.8 %).
             * EA = An_src + sext16(d16). Read .W from mem, sext to 32.
             * Compare 32-bit: a[An_dst] - sext_word. Flags + advance.
             * d16 is loaded from the prefetched ext word via load_imm32. */
            int dst_an = (w >> 9) & 7;
            int src_an = w & 7;
            u16 ext  = mac_read16(cpu->mem, op_pc[i] + 2);
            i32 d16  = (i16)ext;

            emit_advance_flush(&e);                  /* before An read */
            /* EA into a8: a[src_an] + d16. */
            emit_read_g(&e, &rc, G_A(src_an), 8);
            if (d16 >= -128 && d16 <= 127) {
                xt_addi(&e, 8, 8, d16);
            } else {
                emit_load_imm(&e, 11, 12, (u32)d16);
                xt_add(&e, 8, 8, 11);
            }

            emit_l32r_at(&e, 9, lit_off[LITERAL_RAM_BOUNDS],
                         entry_off + e.len);
            xt_and  (&e, 10, 8, 9);
            emit_cache_flush(&e, &rc);   /* before conditional helper */
            i32 op_pc_cmpa = 4, op_cyc_cmpa = 10;
            xt_beqz (&e, 10, (i32)(6u + helper_step_after_flush_undo_size(&rc, op_pc_cmpa, op_cyc_cmpa)));

            emit_helper_step_after_flush_undo(&e, lit_off[HELPER_M68K_STEP],
                                              entry_off, &rc, op_pc_cmpa, op_cyc_cmpa);

            u32 jcmpa_pos = e.len;
            xt_j(&e, 4);

            /* Fast path: read .W, sext, compare. */
            emit_l32r_at(&e, 9, lit_off[ADDR_RAM_BASE],
                         entry_off + e.len);
            xt_add  (&e, 9, 9, 8);
            xt_l8ui (&e, 11, 9, 0);
            xt_l8ui (&e, 12, 9, 1);
            xt_slli (&e, 11, 11, 8);
            xt_or   (&e, 11, 11, 12);    /* a11 = .W */
            /* Sign-extend .W to 32 bits → a8 (source for sub). */
            xt_slli (&e, 8, 11, 16);
            xt_srai (&e, 8, 8, 16);
            emit_read_g(&e, &rc, G_A(dst_an), 9);    /* a9 = dest (full 32) */
            xt_sub  (&e, 10, 9, 8);
            emit_addsub_flags_long_masked(&e, true, true, 8, 9, 10, flags_needed[i]);
            emit_advance(&e, op_pc_cmpa, op_cyc_cmpa);

            u32 cmpa_here = e.len;
            i32 cmpa_off = (i32)(cmpa_here - jcmpa_pos) - 4;
            u32 cw = ((u32)((u32)cmpa_off & 0x3FFFFu) << 6) | 0x06u;
            base[entry_off + jcmpa_pos    ] = (u8)cw;
            base[entry_off + jcmpa_pos + 1] = (u8)(cw >> 8);
            base[entry_off + jcmpa_pos + 2] = (u8)(cw >> 16);

            inline_ops++; done = true;
        } else if (top == 0xB && szf == 1 && ((w >> 8) & 1) == 0 && mode == 5) {
            /* CMP.W (d16,An),Dn — bench-hot 0xBC6D (~6.8 %).
             *   EA = a[an] + sext16(d16); read .W; sext to 32; compare
             *   against d[dn].W shifted to high 16 via the existing
             *   shift trick; emit_addsub_flags_long(sub, keep_x).
             *
             * CMP+Bcc fusion: when followed by Bcc.S LT (the bench tail
             * pattern at 0x03DF58 / 0x03DF5E), skip the R_SR write and
             * compute the condition bit directly from (s, d, r). Saves
             * ~10 ops per fast-path execution. */
            int dn = (w >> 9) & 7;
            int an = w & 7;
            u16 ext = mac_read16(cpu->mem, op_pc[i] + 2);
            i32 d16 = (i16)ext;

            /* Fusion eligibility — next op is Bcc.S BLT/BLE with i8 disp. */
            bool fuse = false;
            int fuse_cc = 0;
            i32 fuse_disp = 0;
            u32 fuse_ft = 0;
            if (i + 1 < n_ops) {
                u16 nw = op_word[i + 1];
                if ((nw >> 12) == 0x6) {
                    int cc = (nw >> 8) & 0xF;
                    i32 disp = (i8)(nw & 0xFF);
                    /* BLT (cc=13) — bench's 0x03DF58 / 0x03DF5E.
                     * BLE (cc=15) — bench's 0x03DF40 (paired with the other
                     *               CMP arm but BLE also possible here). */
                    if ((cc == 6 || cc == 7 || cc == 13 || cc == 15) && disp != 0 && disp != -1) {
                        fuse = true;
                        fuse_cc = cc;
                        fuse_disp = disp;
                        fuse_ft = op_pc[i + 1] + 2;
                    }
                }
            }

            emit_advance_flush(&e);                  /* before An read */
            emit_read_g(&e, &rc, G_A(an), 8);
            if (d16 >= -128 && d16 <= 127) xt_addi(&e, 8, 8, d16);
            else { emit_load_imm32(&e, 11, 12, (u32)d16); xt_add(&e, 8, 8, 11); }

            emit_l32r_at(&e, 9, lit_off[LITERAL_RAM_BOUNDS],
                         entry_off + e.len);
            xt_and  (&e, 10, 8, 9);
            emit_cache_flush(&e, &rc);   /* before conditional helper */
            i32 op_pc_cwd = 4, op_cyc_cwd = 8;
            u32 helper_skip = helper_step_after_flush_undo_size(&rc, op_pc_cwd, op_cyc_cwd);
            u32 helper_bcc_tail_size = fuse ? fused_helper_bcc_tail_size(fuse_cc) : 0;
            xt_beqz (&e, 10, (i32)(6u + helper_skip + helper_bcc_tail_size));
            emit_helper_step_after_flush_undo(&e, lit_off[HELPER_M68K_STEP],
                                              entry_off, &rc, op_pc_cwd, op_cyc_cwd);
            if (fuse) {
                /* Helper-path Bcc tail. The bridge already reloaded R_SR
                 * (CMP wrote it), so emit_cond reads CMP's flags. The
                 * bridge also undid m68k_step's natural advance, so the
                 * accumulator is 0; we fold CMP's 8 + Bcc's 12 into 20. */
                u32 before = e.len;
                emit_cond(&e, fuse_cc);
                emit_bcc_branchless_tail(&e, fuse_ft, fuse_disp, 20);
                /* Sanity: must match the size promised to xt_beqz above. */
                if (e.len - before != helper_bcc_tail_size) {
                    e.overflow = true;   /* abort compile, dispatcher falls back */
                }
            }
            u32 jcwd_pos = e.len;
            xt_j(&e, 4);
            /* fast path */
            emit_l32r_at(&e, 9, lit_off[ADDR_RAM_BASE],
                         entry_off + e.len);
            xt_add  (&e, 9, 9, 8);
            xt_l8ui (&e, 11, 9, 0);
            xt_l8ui (&e, 12, 9, 1);
            xt_slli (&e, 11, 11, 8);
            xt_or   (&e, 11, 11, 12);       /* a11 = .W */
            xt_slli (&e, 8, 11, 16);         /* a8 = s shifted */
            emit_read_g(&e, &rc, G_D(dn), 9);
            xt_slli (&e, 9, 9, 16);          /* a9 = d shifted */
            xt_sub  (&e, 10, 9, 8);
            if (fuse) {
                emit_cmp_cond_fused(&e, fuse_cc, 8, 9, 10);
                emit_bcc_branchless_tail(&e, fuse_ft, fuse_disp, 20);
            } else {
                emit_addsub_flags_long_masked(&e, true, true, 8, 9, 10, flags_needed[i]);
                emit_advance(&e, op_pc_cwd, op_cyc_cwd);
            }

            u32 cwd_here = e.len;
            i32 cwd_off = (i32)(cwd_here - jcwd_pos) - 4;
            u32 cww = ((u32)((u32)cwd_off & 0x3FFFFu) << 6) | 0x06u;
            base[entry_off + jcwd_pos    ] = (u8)cww;
            base[entry_off + jcwd_pos + 1] = (u8)(cww >> 8);
            base[entry_off + jcwd_pos + 2] = (u8)(cww >> 16);

            if (fuse) {
                /* Bcc.S has set cpu->pc directly and absorbed all cycles —
                 * reset accumulator so any subsequent ops (none expected,
                 * Bcc ends the block) don't double-account. */
                g_pc_acc = 0;
                g_cyc_acc = 0;
                i++;   /* skip the Bcc.S we just fused away */
            }

            inline_ops++; done = true;
        } else if (top == 0xB && szf == 1 && ((w >> 8) & 1) == 0 && mode == 2) {
            /* CMP.W (An),Dn — bench-hot 0xBA52 (~6.7 %).
             *   Compute EA in a8 → bounds check → fast path: read .W,
             *   subtract from Dn.W, emit_addsub_flags_long(sub, keep_x);
             *   helper fallback for ROM/devices/odd. */
            int dn = (w >> 9) & 7;
            int an = w & 7;

            /* Fusion eligibility (BLT/BLE.S) — bench's 0x03DF40 ends with
             * CMP.W (A2),D5 + BLE.S +6 (cc=15, 405 K hits). */
            bool fuse = false;
            int fuse_cc = 0;
            i32 fuse_disp = 0;
            u32 fuse_ft = 0;
            if (i + 1 < n_ops) {
                u16 nw = op_word[i + 1];
                if ((nw >> 12) == 0x6) {
                    int cc = (nw >> 8) & 0xF;
                    i32 disp = (i8)(nw & 0xFF);
                    if ((cc == 6 || cc == 7 || cc == 13 || cc == 15) && disp != 0 && disp != -1) {
                        fuse = true;
                        fuse_cc = cc;
                        fuse_disp = disp;
                        fuse_ft = op_pc[i + 1] + 2;
                    }
                }
            }

            emit_advance_flush(&e);                  /* before An read */
            emit_read_g(&e, &rc, G_A(an), 8);
            emit_l32r_at(&e, 9, lit_off[LITERAL_RAM_BOUNDS],
                         entry_off + e.len);
            xt_and  (&e, 10, 8, 9);
            emit_cache_flush(&e, &rc);   /* before conditional helper */
            i32 op_pc_cw = 2, op_cyc_cw = 8;
            u32 helper_skip_cw = helper_step_after_flush_undo_size(&rc, op_pc_cw, op_cyc_cw);
            u32 helper_bcc_tail_cw = fuse ? fused_helper_bcc_tail_size(fuse_cc) : 0;
            xt_beqz (&e, 10, (i32)(6u + helper_skip_cw + helper_bcc_tail_cw));
            /* helper */
            emit_helper_step_after_flush_undo(&e, lit_off[HELPER_M68K_STEP],
                                              entry_off, &rc, op_pc_cw, op_cyc_cw);
            if (fuse) {
                u32 before = e.len;
                emit_cond(&e, fuse_cc);
                /* op_pc_cw = 2 (CMP.W (An),Dn is 2 bytes), op_cyc_cw = 8.
                 * Bcc.S base 12 cycles. Fold: 8 + 12 = 20. */
                emit_bcc_branchless_tail(&e, fuse_ft, fuse_disp, 20);
                if (e.len - before != helper_bcc_tail_cw) {
                    e.overflow = true;
                }
            }
            /* j over fast — backpatched below. */
            u32 j_pos = e.len;
            xt_j    (&e, 4);   /* placeholder */
            /* fast path */
            emit_l32r_at(&e, 9, lit_off[ADDR_RAM_BASE],
                         entry_off + e.len);
            xt_add  (&e, 9, 9, 8);
            xt_l8ui (&e, 11, 9, 0);
            xt_l8ui (&e, 12, 9, 1);
            xt_slli (&e, 11, 11, 8);
            xt_or   (&e, 11, 11, 12);
            xt_slli (&e, 8, 11, 16);          /* a8 = s shifted */
            emit_read_g(&e, &rc, G_D(dn), 9);
            xt_slli (&e, 9, 9, 16);            /* a9 = d shifted */
            xt_sub  (&e, 10, 9, 8);            /* a10 = r shifted */
            if (fuse) {
                emit_cmp_cond_fused(&e, fuse_cc, 8, 9, 10);
                emit_bcc_branchless_tail(&e, fuse_ft, fuse_disp, 20);
            } else {
                emit_addsub_flags_long_masked(&e, true, true, 8, 9, 10, flags_needed[i]);
                emit_advance(&e, op_pc_cw, op_cyc_cw);
            }
            /* backpatch the j-over-helper to land here. */
            u32 here = e.len;
            i32 j_off = (i32)(here - j_pos) - 4;
            u32 jw = ((u32)((u32)j_off & 0x3FFFFu) << 6) | 0x06u;
            base[entry_off + j_pos    ] = (u8)jw;
            base[entry_off + j_pos + 1] = (u8)(jw >> 8);
            base[entry_off + j_pos + 2] = (u8)(jw >> 16);

            if (fuse) {
                g_pc_acc = 0;
                g_cyc_acc = 0;
                i++;
            }

            inline_ops++; done = true;
        } else if (top == 0x3 && ((w >> 6) & 7) == 0 && mode == 6) {
            /* MOVE.W (d8,An,Xn),Dn — indexed source, the bench-hot 0x3A30.
             * Same fast-path / helper-fallback shape as the (An),Dn arm
             * below, but with the EA computed from An + sext(d8) + Xn. */
            int dn   = (w >> 9) & 7;
            int an   = w & 7;
            u16 ext  = mac_read16(cpu->mem, op_pc[i] + 2);
            int ireg = (ext >> 12) & 7;
            bool index_is_an = (ext & 0x8000) != 0;
            bool index_is_long = (ext & 0x0800) != 0;
            i32 disp = (i8)(ext & 0xFF);

            emit_advance_flush(&e);                  /* before An/Xn reads */
            /* 1. Compute EA into a8. Reuse a13 sext if memoized. */
            emit_read_g(&e, &rc, G_A(an), 8);
            int g_index = index_is_an ? G_A(ireg) : G_D(ireg);
            if (!index_is_long) {
                if (g_sext_valid && g_sext_src_reg == g_index) {
                    xt_add(&e, 8, 8, 13);
                } else {
                    emit_read_g(&e, &rc, g_index, 13);
                    xt_slli(&e, 13, 13, 16);
                    xt_srai(&e, 13, 13, 16);
                    g_sext_src_reg = g_index;
                    g_sext_valid = true;
                    xt_add(&e, 8, 8, 13);
                }
            } else {
                emit_read_g(&e, &rc, g_index, 9);
                xt_add(&e, 8, 8, 9);
            }
            xt_addi (&e, 8, 8, disp);

            /* 2. Bounds check. */
            emit_l32r_at(&e, 9, lit_off[LITERAL_RAM_BOUNDS],
                         entry_off + e.len);
            xt_and  (&e, 10, 8, 9);

            /* 3. beqz a10, fast_path. rel = 3+9+3 = 15. */
            emit_cache_flush(&e, &rc);   /* before conditional helper */
            i32 op_pc_ix = 4, op_cyc_ix = 8;
            xt_beqz (&e, 10, (i32)(6u + helper_step_after_flush_undo_size(&rc, op_pc_ix, op_cyc_ix)));

            /* 4. Helper path (mov + l32r + callx0 + undo + reload). */
            emit_helper_step_after_flush_undo(&e, lit_off[HELPER_M68K_STEP],
                                              entry_off, &rc, op_pc_ix, op_cyc_ix);

            /* 5. j past fast path — backpatched after the fast path is emitted. */
            u32 jix_pos = e.len;
            xt_j    (&e, 4);

            /* 6. Fast path. */
            emit_l32r_at(&e, 9, lit_off[ADDR_RAM_BASE],
                         entry_off + e.len);
            xt_add  (&e, 9, 9, 8);
            xt_l8ui (&e, 10, 9, 0);
            xt_l8ui (&e, 11, 9, 1);
            xt_slli (&e, 10, 10, 8);
            xt_or   (&e, 10, 10, 11);
            emit_read_g(&e, &rc, G_D(dn), 11);
            xt_srli (&e, 11, 11, 16);
            xt_slli (&e, 11, 11, 16);
            xt_or   (&e, 11, 11, 10);
            emit_write_g(&e, &rc, G_D(dn), 11);
            if (!flags_dead[i]) {
                xt_slli (&e, 8, 10, 16);
                emit_logic_flags(&e, 8);
            }
            emit_advance(&e, op_pc_ix, op_cyc_ix);  /* 4 bytes: opcode + ext */
            { u32 jh = e.len; i32 jo = (i32)(jh - jix_pos) - 4;
              u32 jw = ((u32)((u32)jo & 0x3FFFFu) << 6) | 0x06u;
              base[entry_off + jix_pos    ] = (u8)jw;
              base[entry_off + jix_pos + 1] = (u8)(jw >> 8);
              base[entry_off + jix_pos + 2] = (u8)(jw >> 16); }

            inline_ops++; done = true;
        } else if (top == 0x3 && ((w >> 6) & 7) == 0 && mode == 2) {
            /* MOVE.W (An),Dn — guest-RAM fast path with helper fallback.
             *
             *   bounds:   a8 = a[an]; a10 = a8 & RAM_BOUNDS
             *   beqz a10, FAST       (in RAM, aligned → fast)
             *   helper:   CALLX0 m68k_step(cpu)
             *   j END
             *   FAST:     a10 = read_w(a8); store low-16 to d[dn]; flags;
             *             advance pc/cycles
             *   END
             *
             * Sizes are pre-computed so no back-patching is needed. */
            int dn = (w >> 9) & 7;
            int an = w & 7;

            emit_advance_flush(&e);                  /* before An read */
            /* 1. Bounds check. Read An into-or-in a8: if An is cached, the
             * returned slot (a4..a7) is the source for the AND below — no
             * mov needed. */
            u8 an_reg = emit_read_g_in(&e, &rc, G_A(an), 8);
            emit_l32r_at(&e, 9, lit_off[LITERAL_RAM_BOUNDS],
                         entry_off + e.len);
            xt_and  (&e, 10, an_reg, 9);

            /* 2. beqz a10, fast_path. Fast path defers PC/cyc advance into
             * the compile-time accumulator (saves 6 LX7 ops per execution);
             * the helper bridge undoes m68k_step's natural advance so the
             * next flush adds the accumulator just once. */
            i32 op_pc_delta = 2, op_cyc = 8;
            emit_cache_flush(&e, &rc);   /* before conditional helper */
            xt_beqz (&e, 10, (i32)(6u + helper_step_after_flush_undo_size(&rc, op_pc_delta, op_cyc)));

            /* 3. Helper path (mov + l32r + callx0 + undo PC/cyc + sr_reload + cache_reload). */
            emit_helper_step_after_flush_undo(&e, lit_off[HELPER_M68K_STEP],
                                              entry_off, &rc, op_pc_delta, op_cyc);

            /* 4. j past fast path — backpatched after fast path. */
            u32 jad_pos = e.len;
            xt_j    (&e, 4);

            /* 5. Fast path. NOTE: emit_helper_step_after_flush emitted a
             * cache_reload, but the reload only runs on the helper branch.
             * Fast path's cache state is unchanged from pre-bounds-check,
             * so An is still in `an_reg`. */
            emit_l32r_at(&e, 9, lit_off[ADDR_RAM_BASE],
                         entry_off + e.len);
            xt_add  (&e, 9, 9, an_reg);
            xt_l8ui (&e, 10, 9, 0);              /* high byte */
            xt_l8ui (&e, 11, 9, 1);              /* low byte  */
            xt_slli (&e, 10, 10, 8);
            xt_or   (&e, 10, 10, 11);            /* a10 = .W */
            /* write low 16 of Dn, preserve high 16 — do it in-place on the
             * cache slot if Dn is cached. */
            int dn_xt = cache_lookup(&rc, G_D(dn));
            u8 dn_reg = (dn_xt >= 0) ? (u8)dn_xt : 11;
            if (dn_xt < 0) {
                emit_read_g(&e, &rc, G_D(dn), 11);   /* l32i a11, ... */
            }
            xt_srli (&e, dn_reg, dn_reg, 16);
            xt_slli (&e, dn_reg, dn_reg, 16);
            xt_or   (&e, dn_reg, dn_reg, 10);
            if (dn_xt >= 0) {
                /* in-place on cache slot — mark dirty */
                for (int s = 0; s < rc.active; s++)
                    if (rc.guest[s] == (u8)G_D(dn)) { rc.dirty |= (u16)(1u << s); break; }
            } else {
                emit_write_g(&e, &rc, G_D(dn), 11);
            }
            /* flags: shift .W to high 16 so emit_logic_flags' bit-31 N
             * gives bit-15 of the word. */
            if (!flags_dead[i]) {
                xt_slli (&e, 8, 10, 16);
                emit_logic_flags(&e, 8);
            }
            /* Defer the op's PC/cyc into the compile-time accumulator;
             * helper bridge has already undone its m68k_step advance. */
            emit_advance(&e, op_pc_delta, op_cyc);
            { u32 jh = e.len; i32 jo = (i32)(jh - jad_pos) - 4;
              u32 jw = ((u32)((u32)jo & 0x3FFFFu) << 6) | 0x06u;
              base[entry_off + jad_pos    ] = (u8)jw;
              base[entry_off + jad_pos + 1] = (u8)(jw >> 8);
              base[entry_off + jad_pos + 2] = (u8)(jw >> 16); }

            inline_ops++; done = true;
        } else if (top == 0x4 && (w & 0xFB80) == 0x4880
                   && ((w >> 10) & 1) == 0 && mode == 2) {
            /* MOVEM.W/.L <reglist>,(An) — boot's 0x48D0 (.W) at 131 K.
             * Store regs to memory at An, An unchanged. Bit order: bit 0=D0,
             * bit 15=A7. Length 4, cycles 20. */
            int an = w & 7;
            int sz = (w & 0x40) ? 4 : 2;
            u16 list = mac_read16(cpu->mem, op_pc[i] + 2);
            int n_regs = __builtin_popcount(list);
            int threshold = (sz == 4) ? 5 : 12;

            if (n_regs >= 1 && n_regs <= threshold) {
                emit_advance_flush(&e);
                emit_read_g(&e, &rc, G_A(an), 8);
                emit_l32r_at(&e, 9, lit_off[LITERAL_RAM_BOUNDS],
                             entry_off + e.len);
                xt_addi(&e, 12, 8, n_regs * sz - 1);
                xt_or  (&e, 10, 8, 12);
                xt_and (&e, 10, 10, 9);
                emit_cache_flush(&e, &rc);
                i32 op_pc_msm = 4, op_cyc_msm = 20;
                /* Helper bridge: route MMIO bounds-fail to the appropriate
                 * fast helper — sz=.W → movem_w_to_mem, sz=.L → movem_l_to_mem. */
                literal_id mmio_helper = (sz == 2) ? HELPER_JIT_MOVEM_W_TO_MEM
                                                   : HELPER_JIT_MOVEM_L_TO_MEM;
                u32 msm_bridge_size = emit_movem_fast_bridge_size((u32)list, &rc);
                xt_beqz(&e, 10, (i32)(6u + msm_bridge_size));
                emit_load_imm(&e, 8, 9, (u32)list);
                emit_jit_fast_helper(&e, 8, an, lit_off[mmio_helper],
                                     entry_off, &rc);
                (void)op_pc_msm; (void)op_cyc_msm;
                u32 jmsm_pos = e.len;
                xt_j(&e, 4);
                /* Fast path: write each reg as sz BE bytes. */
                emit_l32r_at(&e, 9, lit_off[ADDR_RAM_BASE],
                             entry_off + e.len);
                xt_add(&e, 9, 9, 8);
                int byte_off = 0;
                for (int b = 0; b < 16; b++) {
                    if (!(list & (1 << b))) continue;
                    int g_reg = (b < 8) ? G_D(b) : G_A(b - 8);
                    emit_read_g(&e, &rc, g_reg, 10);
                    if (sz == 4) {
                        xt_extui(&e, 11, 10, 24, 7); xt_s8i(&e, 11, 9, (u8)(byte_off + 0));
                        xt_extui(&e, 11, 10, 16, 7); xt_s8i(&e, 11, 9, (u8)(byte_off + 1));
                        xt_extui(&e, 11, 10,  8, 7); xt_s8i(&e, 11, 9, (u8)(byte_off + 2));
                        xt_extui(&e, 11, 10,  0, 7); xt_s8i(&e, 11, 9, (u8)(byte_off + 3));
                    } else {
                        xt_extui(&e, 11, 10, 8, 7); xt_s8i(&e, 11, 9, (u8)(byte_off + 0));
                        xt_extui(&e, 11, 10, 0, 7); xt_s8i(&e, 11, 9, (u8)(byte_off + 1));
                    }
                    byte_off += sz;
                }
                emit_advance(&e, op_pc_msm, op_cyc_msm);

                u32 here_msm = e.len;
                i32 jo_msm = (i32)(here_msm - jmsm_pos) - 4;
                u32 jw_msm = ((u32)((u32)jo_msm & 0x3FFFFu) << 6) | 0x06u;
                base[entry_off + jmsm_pos    ] = (u8)jw_msm;
                base[entry_off + jmsm_pos + 1] = (u8)(jw_msm >> 8);
                base[entry_off + jmsm_pos + 2] = (u8)(jw_msm >> 16);
                sext_memo_invalidate();
                inline_ops++; done = true;
            } else if (n_regs >= 1) {
                /* Large reglist: use fast helper instead of m68k_step. */
                literal_id mmio_helper = (sz == 2) ? HELPER_JIT_MOVEM_W_TO_MEM
                                                   : HELPER_JIT_MOVEM_L_TO_MEM;
                emit_advance_flush(&e);
                emit_cache_flush(&e, &rc);
                emit_load_imm(&e, 8, 9, (u32)list);
                emit_jit_fast_helper(&e, 8, an, lit_off[mmio_helper],
                                     entry_off, &rc);
                emit_advance(&e, 4, 20);
                inline_ops++; done = true;
            }
        } else if (top == 0x4 && (w & 0xFB80) == 0x4880
                   && ((w >> 10) & 1) == 0 && mode == 4) {
            /* MOVEM.W/.L <reglist>,-(An) — boot's 0x48E1 (.L) at 131 K.
             * Pre-decrement An by N*sz then write regs at consecutive
             * offsets from the new An. Bit order is REVERSED for -(An):
             * bit 0=A7, bit 15=D0. The lowest-numbered register set
             * (smallest bit position) ends up at the HIGHEST address. */
            int an = w & 7;
            int sz = (w & 0x40) ? 4 : 2;
            u16 list = mac_read16(cpu->mem, op_pc[i] + 2);
            int n_regs = __builtin_popcount(list);
            int threshold = (sz == 4) ? 5 : 12;

            if (n_regs >= 1 && n_regs <= threshold) {
                emit_advance_flush(&e);
                emit_read_g(&e, &rc, G_A(an), 8);
                /* New An = An - N*sz. */
                xt_addi(&e, 8, 8, -(n_regs * sz));
                emit_l32r_at(&e, 9, lit_off[LITERAL_RAM_BOUNDS],
                             entry_off + e.len);
                xt_addi(&e, 12, 8, n_regs * sz - 1);
                xt_or  (&e, 10, 8, 12);
                xt_and (&e, 10, 10, 9);
                emit_cache_flush(&e, &rc);
                i32 op_pc_pdm = 4, op_cyc_pdm = 20;
                /* Helper bridge: route MMIO bounds-fail to the fast helper.
                 * Only .L has a fast helper (m68k_jit_movem_l_predec_from_regs);
                 * for .W -(An) we keep m68k_step. */
                if (sz == 4) {
                    u32 pdm_bridge_size = emit_movem_fast_bridge_size((u32)list, &rc);
                    xt_beqz(&e, 10, (i32)(6u + pdm_bridge_size));
                    emit_load_imm(&e, 8, 9, (u32)list);
                    emit_jit_fast_helper(&e, 8, an,
                                         lit_off[HELPER_JIT_MOVEM_L_PREDEC],
                                         entry_off, &rc);
                } else {
                    xt_beqz(&e, 10, (i32)(6u + helper_step_after_flush_undo_size(&rc, op_pc_pdm, op_cyc_pdm)));
                    emit_helper_step_after_flush_undo(&e, lit_off[HELPER_M68K_STEP],
                                                      entry_off, &rc, op_pc_pdm, op_cyc_pdm);
                }
                u32 jpdm_pos = e.len;
                xt_j(&e, 4);
                /* Fast path: iterate bits high-to-low (= regs low-to-high
                 * since bit i = reg 15-i), writing at increasing offsets
                 * from the (decremented) An. */
                emit_l32r_at(&e, 9, lit_off[ADDR_RAM_BASE],
                             entry_off + e.len);
                xt_add(&e, 9, 9, 8);
                int byte_off = 0;
                for (int b = 15; b >= 0; b--) {
                    if (!(list & (1 << b))) continue;
                    int reg_idx = 15 - b;
                    int g_reg = (reg_idx < 8) ? G_D(reg_idx) : G_A(reg_idx - 8);
                    emit_read_g(&e, &rc, g_reg, 10);
                    if (sz == 4) {
                        xt_extui(&e, 11, 10, 24, 7); xt_s8i(&e, 11, 9, (u8)(byte_off + 0));
                        xt_extui(&e, 11, 10, 16, 7); xt_s8i(&e, 11, 9, (u8)(byte_off + 1));
                        xt_extui(&e, 11, 10,  8, 7); xt_s8i(&e, 11, 9, (u8)(byte_off + 2));
                        xt_extui(&e, 11, 10,  0, 7); xt_s8i(&e, 11, 9, (u8)(byte_off + 3));
                    } else {
                        xt_extui(&e, 11, 10, 8, 7); xt_s8i(&e, 11, 9, (u8)(byte_off + 0));
                        xt_extui(&e, 11, 10, 0, 7); xt_s8i(&e, 11, 9, (u8)(byte_off + 1));
                    }
                    byte_off += sz;
                }
                /* Write decremented An back. */
                emit_write_g(&e, &rc, G_A(an), 8);
                emit_advance(&e, op_pc_pdm, op_cyc_pdm);

                u32 here_pdm = e.len;
                i32 jo_pdm = (i32)(here_pdm - jpdm_pos) - 4;
                u32 jw_pdm = ((u32)((u32)jo_pdm & 0x3FFFFu) << 6) | 0x06u;
                base[entry_off + jpdm_pos    ] = (u8)jw_pdm;
                base[entry_off + jpdm_pos + 1] = (u8)(jw_pdm >> 8);
                base[entry_off + jpdm_pos + 2] = (u8)(jw_pdm >> 16);
                sext_memo_invalidate();
                inline_ops++; done = true;
            } else if (n_regs >= 1 && sz == 4) {
                /* Large reglist .L: use fast helper. */
                emit_advance_flush(&e);
                emit_cache_flush(&e, &rc);
                emit_load_imm(&e, 8, 9, (u32)list);
                emit_jit_fast_helper(&e, 8, an,
                                     lit_off[HELPER_JIT_MOVEM_L_PREDEC],
                                     entry_off, &rc);
                emit_advance(&e, 4, 20);
                inline_ops++; done = true;
            }
        } else if (top == 0x4 && (w & 0xFB80) == 0x4880
                   && ((w >> 10) & 1) == 1 && (w & 0x40) != 0 && mode == 3) {
            /* MOVEM.L (An)+,<reglist> — boot's 0x4CD8 at 262 K execs.
             *
             * Inline-gated by popcount(list) <= 4 to keep emit size under
             * the helper's effective cost (helper = 64 LX7, per-reg
             * inline body = 11 ops × 3 bytes). Larger reglists fall
             * through to the helper for break-even.
             *
             * Length = 4 (opcode + list word). Cycles = 16 + m68k_step
             * base 4 = 20 (matches interp; constant regardless of N). */
            int an = w & 7;
            u16 list = mac_read16(cpu->mem, op_pc[i] + 2);
            int n_regs = __builtin_popcount(list);

            if (n_regs >= 1 && n_regs <= 5) {
                emit_advance_flush(&e);
                emit_read_g(&e, &rc, G_A(an), 8);    /* a8 = An (start) */
                emit_l32r_at(&e, 9, lit_off[LITERAL_RAM_BOUNDS],
                             entry_off + e.len);
                /* OR-bounds-check: a8 | (a8 + N*4 - 1). Both must be in RAM
                 * → result & RAM_BOUNDS == 0. */
                xt_addi(&e, 12, 8, n_regs * 4 - 1);
                xt_or  (&e, 10, 8, 12);
                xt_and (&e, 10, 10, 9);
                emit_cache_flush(&e, &rc);
                i32 op_pc_mvm = 4, op_cyc_mvm = 20;
                /* Helper bridge: route MMIO bounds-fail to the fast
                 * MOVEM helper instead of m68k_step (saves ~50 LX7 per
                 * VIA-targeted MOVEM in boot). */
                u32 mvm_bridge_size = emit_movem_fast_bridge_size((u32)list, &rc);
                xt_beqz(&e, 10, (i32)(6u + mvm_bridge_size));
                emit_load_imm(&e, 8, 9, (u32)list);
                emit_jit_fast_helper(&e, 8, an,
                                     lit_off[HELPER_JIT_MOVEM_L_POSTINC],
                                     entry_off, &rc);
                u32 jmvm_pos = e.len;
                xt_j(&e, 4);
                /* Fast path. a8 still = An (the beqz skipped past the
                 * bridge that would have clobbered it). */
                emit_l32r_at(&e, 9, lit_off[ADDR_RAM_BASE],
                             entry_off + e.len);
                xt_add(&e, 9, 9, 8);                  /* a9 = ram + addr */
                int byte_off = 0;
                for (int b = 0; b < 16; b++) {
                    if (!(list & (1 << b))) continue;
                    /* Read 4 BE bytes from a9+byte_off into a10. */
                    xt_l8ui (&e, 11, 9, (u8)(byte_off + 0));
                    xt_l8ui (&e, 12, 9, (u8)(byte_off + 1));
                    xt_slli (&e, 10, 11, 24);
                    xt_slli (&e, 12, 12, 16);
                    xt_or   (&e, 10, 10, 12);
                    xt_l8ui (&e, 11, 9, (u8)(byte_off + 2));
                    xt_l8ui (&e, 12, 9, (u8)(byte_off + 3));
                    xt_slli (&e, 11, 11, 8);
                    xt_or   (&e, 10, 10, 11);
                    xt_or   (&e, 10, 10, 12);
                    int g_reg = (b < 8) ? G_D(b) : G_A(b - 8);
                    emit_write_g(&e, &rc, g_reg, 10);
                    byte_off += 4;
                }
                /* An += N * 4. */
                xt_addi(&e, 8, 8, n_regs * 4);
                emit_write_g(&e, &rc, G_A(an), 8);
                emit_advance(&e, op_pc_mvm, op_cyc_mvm);

                u32 here_mvm = e.len;
                i32 jo_mvm = (i32)(here_mvm - jmvm_pos) - 4;
                u32 jw_mvm = ((u32)((u32)jo_mvm & 0x3FFFFu) << 6) | 0x06u;
                base[entry_off + jmvm_pos    ] = (u8)jw_mvm;
                base[entry_off + jmvm_pos + 1] = (u8)(jw_mvm >> 8);
                base[entry_off + jmvm_pos + 2] = (u8)(jw_mvm >> 16);

                sext_memo_invalidate();
                inline_ops++; done = true;
            } else if (n_regs >= 1) {
                /* Large reglist .L: use fast helper. */
                emit_advance_flush(&e);
                emit_cache_flush(&e, &rc);
                emit_load_imm(&e, 8, 9, (u32)list);
                emit_jit_fast_helper(&e, 8, an,
                                     lit_off[HELPER_JIT_MOVEM_L_POSTINC],
                                     entry_off, &rc);
                emit_advance(&e, 4, 20);
                inline_ops++; done = true;
            }
            /* else: empty reglist — fall through to helper. */
        } else if (top == 0x4 && ((w >> 6) & 7) == 7
                   && (mode == 2 || mode == 5 || mode == 6
                       || (mode == 7 && (w & 7) <= 1))) {
            /* LEA <ea>,An */
            int sr = w & 7;
            u16 ext   = (mode == 5 || mode == 6 || (mode == 7 && sr == 0))
                      ? mac_read16(cpu->mem, op_pc[i] + 2) : 0;
            u32 ext32 = (mode == 7 && sr == 1)
                      ? mac_read32(cpu->mem, op_pc[i] + 2) : 0;
            emit_lea(&e, (w >> 9) & 7, mode, sr, ext, ext32, &rc);
            inline_ops++; done = true;
        } else if (top == 0x6 && (((w>>8)&0xF) != 1)
                   && (w & 0xFF) != 0x00 && (w & 0xFF) != 0xFF) {
            /* All Bcc.S except BSR (cc 1). Aggressive — accepts the
             * --diff-jit divergence pattern that affects some less-hot
             * conditions, traded for the bench/boot perf win. (Loop
             * internalisation was tried and hung boot — interrupt-
             * dependent wait-loops never exit when re-entering the
             * dispatcher is skipped.) */
            emit_branch(&e, op_pc[i], w);
            inline_ops++; done = true;
        } else if (top == 0x5 && szf == 3 && mode == 1
                   && (((w >> 8) & 0xF) == 1 || ((w >> 8) & 0xF) == 6)) {
            /* DBF / DBNE Dn, disp16 — boot's two big DBcc helpers.
             *   DBF (cc=1):   always decrement+branch.
             *   DBNE (cc=6):  if Z==0 (NE true), fall through.
             *                 Else (Z==1), decrement+branch.
             *
             *   if !cond_true: Dn.W = Dn.W - 1;
             *                  pc = (Dn.W != -1) ? op_pc+2+disp16 : op_pc+4;
             *   else:          pc = op_pc + 4;
             *   cycles += 14 (unconditional). */
            int cc = (w >> 8) & 0xF;
            int dn = w & 7;
            i16 disp16 = (i16)mac_read16(cpu->mem, op_pc[i] + 2);
            u32 ft = op_pc[i] + 4;
            i32 disp_for_tail = (i32)disp16 - 2;
            bool is_dbne = (cc == 6);

            i32 extra_cyc = g_cyc_acc;
            g_pc_acc = 0;
            g_cyc_acc = 0;

            /* 1. Read Dn into a11 (always — needed for decrement and cond). */
            emit_read_g(&e, &rc, G_D(dn), 11);
            /* 2. Compute new_dn in a10 (= Dn with .W decremented). */
            xt_addi (&e, 9, 11, -1);
            xt_srli (&e, 10, 11, 16);
            xt_slli (&e, 10, 10, 16);
            xt_extui(&e, 12, 9, 0, 15);
            xt_or   (&e, 10, 10, 12);
            /* 3. For DBNE only: if Z=0 (NE true), restore a10 = Dn_old to skip dec. */
            if (is_dbne) {
                xt_extui(&e, 13, R_SR, 2, 0);   /* a13 = Z (0 or 1) */
                xt_bnez (&e, 13, 6);            /* if Z != 0 (NE false), keep a10 */
                xt_mov  (&e, 10, 11);           /* Z == 0 (NE true) → restore old */
            }
            /* 4. Write Dn from a10. */
            emit_write_g(&e, &rc, G_D(dn), 10);
            /* 5. Cond_branch compute:
             *    DBF: cond = (old_dn.W != 0) ? 1 : 0.
             *    DBNE: cond = (Z=1) AND (old_dn.W != 0). */
            xt_extui(&e, 12, 11, 0, 15);        /* a12 = old_dn.W */
            if (is_dbne) {
                /* a13 still holds Z from step 3. */
                xt_movi(&e, 8, 0);              /* default cond = 0 */
                xt_beqz(&e, 13, 12);            /* Z==0 → cond stays 0 (skip 3 ops) */
                xt_movi(&e, 8, 1);              /* Z=1 → assume cond = 1 */
                xt_bnez(&e, 12, 6);             /* not done → skip */
                xt_movi(&e, 8, 0);              /* done → cond = 0 */
            } else {
                xt_movi(&e, 8, 1);              /* default cond = 1 (not done) */
                xt_bnez(&e, 12, 6);             /* if old_dn.W != 0, keep cond=1 */
                xt_movi(&e, 8, 0);              /* old_dn.W == 0 → cond = 0 */
            }
            /* 6. Branchless PC + unconditional cycle update. */
            xt_movi (&e, 10, 0);
            xt_sub  (&e, 9, 10, 8);             /* mask = -cond */
            if (g_pc_lit_valid && ft == g_pc_lit_val) {
                emit_l32r_at(&e, 10, g_pc_lit_off, g_pc_lit_entry_off + e.len);
            } else {
                emit_load_imm32(&e, 10, 11, ft);
            }
            if (disp_for_tail >= -128 && disp_for_tail <= 127) {
                xt_addi(&e, 12, 10, (i32)disp_for_tail);
            } else {
                u32 taken = ft + (u32)disp_for_tail;
                emit_load_imm32(&e, 12, 11, taken);
            }
            xt_xor (&e, 13, 10, 12);
            xt_and (&e, 13, 13, 9);
            xt_xor (&e, 10, 10, 13);
            xt_s32i(&e, 10, R_CPU, OFF_PC);
            xt_l32i(&e, 11, R_CPU, OFF_CYCLES);
            i32 db_base_cyc = 14 + extra_cyc;
            if (db_base_cyc >= -128 && db_base_cyc <= 127) {
                xt_addi(&e, 11, 11, db_base_cyc);
            } else {
                emit_load_imm(&e, 10, 12, (u32)db_base_cyc);
                xt_add(&e, 11, 11, 10);
            }
            xt_s32i(&e, 11, R_CPU, OFF_CYCLES);

            sext_memo_invalidate();
            inline_ops++; done = true;
        } else if ((top == 0xD || top == 0x9) && szf == 1
                   && !((w >> 8) & 1) && mode == 0) {
            /* ADD.W / SUB.W Dm,Dn — boot-warm 0xD441 (~9 K). */
            bool is_sub = (top == 0x9);
            int dn = (w >> 9) & 7;
            int dm = w & 7;
            emit_read_g(&e, &rc, G_D(dm), 8);
            emit_read_g(&e, &rc, G_D(dn), 11);
            xt_slli(&e, 8, 8, 16);
            xt_slli(&e, 9, 11, 16);
            if (is_sub) xt_sub(&e, 10, 9, 8);
            else        xt_add(&e, 10, 9, 8);
            xt_srli(&e, 11, 11, 16);
            xt_slli(&e, 11, 11, 16);
            xt_extui(&e, 12, 10, 16, 15);
            xt_or  (&e, 11, 11, 12);
            emit_write_g(&e, &rc, G_D(dn), 11);
            if (!flags_dead[i]) {
                emit_addsub_flags_long(&e, is_sub, false);
            }
            emit_advance(&e, 2, 4);
            inline_ops++; done = true;
        } else if (top == 0x4 && ((w >> 8) & 0xF) == 0xA && szf == 2 && mode == 0) {
            /* TST.L Dn — bench-warm 0x4A80/0x4A81 (~4 K). N/Z from Dn,
             * V=C=0, X preserved. */
            int dn = w & 7;
            emit_read_g(&e, &rc, G_D(dn), 8);
            if (!flags_dead[i]) emit_logic_flags(&e, 8);
            emit_advance(&e, 2, 4);
            inline_ops++; done = true;
        } else if (top == 0xB && szf == 2 && !((w >> 8) & 1) && mode == 0) {
            /* CMP.L Dm,Dn */
            emit_cmp_l_dd(&e, (w >> 9) & 7, w & 7, &rc, flags_needed[i]);
            inline_ops++; done = true;
        } else if (top == 0x9 && szf == 2 && !((w >> 8) & 1) && mode == 0) {
            /* SUB.L Dm,Dn */
            emit_sub_l_dd(&e, (w >> 9) & 7, w & 7, flags_dead[i], &rc);
            inline_ops++; done = true;
        } else if (top == 0xC && szf == 2 && !((w >> 8) & 1) && mode == 0) {
            /* AND.L Dm,Dn  (result -> Dn) */
            emit_logic_l_dd(&e, (w >> 9) & 7, w & 7, (w >> 9) & 7, false, &rc);
            inline_ops++; done = true;
        } else if (top == 0x8 && szf == 2 && !((w >> 8) & 1) && mode == 0) {
            /* OR.L Dm,Dn  (result -> Dn).  Boot-hot 0x8087/0x8C85/0x8E86. */
            emit_logic_l_dd_kind(&e, (w >> 9) & 7, w & 7, (w >> 9) & 7,
                                 0, flags_dead[i], &rc);
            inline_ops++; done = true;
        } else if (top == 0xB && szf == 2 && ((w >> 8) & 1) && mode == 0) {
            /* EOR.L Dn,Dm  (result -> Dm) */
            emit_logic_l_dd(&e, (w >> 9) & 7, w & 7, w & 7, true, &rc);
            inline_ops++; done = true;
        } else if (top == 0x0 && !((w >> 8) & 1) && szf == 2 && mode == 0
                   && ((w >> 9) & 7) != 4 && ((w >> 9) & 7) != 7) {
            /* ORI/ANDI/SUBI/ADDI/EORI/CMPI.L #imm32,Dn */
            u32 imm = mac_read32(cpu->mem, op_pc[i] + 2);
            emit_immalu_l_dn(&e, w & 7, imm, (w >> 9) & 7, &rc);
            inline_ops++; done = true;
        } else if (top == 0x0 && !((w >> 8) & 1) && (szf == 0 || szf == 1) && mode == 0
                   && (((w >> 9) & 7) == 0 || ((w >> 9) & 7) == 1 || ((w >> 9) & 7) == 5)) {
            /* ORI/ANDI/EORI .B/.W #imm,Dn. Skip the SR/CCR forms (mode 7,
             * reg 4 — handled elsewhere). */
            u32 imm = mac_read16(cpu->mem, op_pc[i] + 2);
            int size = (szf == 0) ? 1 : 2;
            emit_immlogic_bw_dn(&e, w & 7, imm, size, (w >> 9) & 7, &rc);
            inline_ops++; done = true;
        } else if (top == 0x0 && !((w >> 8) & 1) && (szf == 0 || szf == 1) && mode == 0
                   && (((w >> 9) & 7) == 2 || ((w >> 9) & 7) == 3 || ((w >> 9) & 7) == 6)) {
            /* SUBI/ADDI/CMPI .B/.W #imm,Dn — arithmetic immediates. */
            u32 imm = mac_read16(cpu->mem, op_pc[i] + 2);
            int size = (szf == 0) ? 1 : 2;
            emit_immarith_bw_dn(&e, w & 7, imm, size, (w >> 9) & 7, &rc);
            inline_ops++; done = true;
        } else if (top == 0x0 && !((w >> 8) & 1) && ((w >> 9) & 7) == 4
                   && szf == 0 && mode == 5) {
            /* BTST #imm,(d16,An) — boot-hot 0x082D at 214 K execs.
             * Byte EA: bit & 7 selects the bit to test. Sets only Z.
             * Length = 6 (opcode + imm word + d16 word), cycles = 8. */
            int an = w & 7;
            u16 imm_word = mac_read16(cpu->mem, op_pc[i] + 2);
            int bit = imm_word & 7;
            i16 d16 = (i16)mac_read16(cpu->mem, op_pc[i] + 4);

            emit_advance_flush(&e);
            emit_read_g(&e, &rc, G_A(an), 8);
            if (d16 >= -128 && d16 <= 127) {
                xt_addi(&e, 8, 8, d16);
            } else {
                emit_load_imm32(&e, 11, 12, (u32)(i32)d16);
                xt_add(&e, 8, 8, 11);
            }
            emit_l32r_at(&e, 9, lit_off[LITERAL_RAM_BOUNDS],
                         entry_off + e.len);
            xt_and(&e, 10, 8, 9);
            emit_cache_flush(&e, &rc);
            i32 op_pc_bt = 6, op_cyc_bt = 8;
            /* Custom MMIO helper bridge (mirrors the ORI.B M6.42 pattern). */
            u32 btst_bridge_size = 3*7;  /* mov, l32r, callx0, sr_reload, 3 setup */
            if (g_sr_dirty) btst_bridge_size += 3;
            btst_bridge_size += (u32)rc.active * 3;
            xt_beqz(&e, 10, (i32)(6u + btst_bridge_size));
            /* --- Custom MMIO BTST helper bridge. --- */
            sext_memo_invalidate();
            emit_sr_flush(&e);
            xt_s32i(&e, 8, R_CPU, OFF_JIT_ARG1);   /* arg1 = addr */
            xt_movi(&e, 9, bit);
            xt_s32i(&e, 9, R_CPU, OFF_JIT_ARG2);   /* arg2 = bit position */
            xt_mov (&e, R_ARG, R_CPU);
            emit_l32r_at(&e, R_HELP, lit_off[HELPER_JIT_BTST_B_MMIO],
                         entry_off + e.len);
            xt_callx0(&e, R_HELP);
            emit_sr_reload(&e);
            emit_cache_reload(&e, &rc);
            /* --- end custom bridge --- */
            u32 jbt_pos = e.len;
            xt_j(&e, 4);
            /* Fast path: read byte, extract bit, update Z. */
            emit_l32r_at(&e, 9, lit_off[ADDR_RAM_BASE],
                         entry_off + e.len);
            xt_add(&e, 9, 9, 8);
            xt_l8ui(&e, 10, 9, 0);                  /* a10 = byte */
            xt_extui(&e, 11, 10, (u8)bit, 0);       /* a11 = (byte >> bit) & 1 */
            /* Clear R_SR.Z (bit 2), then OR in Z if a11 == 0. */
            xt_movi(&e, 12, -5);                    /* ~0x04 (sign-extended) */
            xt_and(&e, R_SR, R_SR, 12);
            xt_movi(&e, 12, 0x04);
            xt_bnez(&e, 11, 6);                     /* if bit set, skip OR */
            xt_or(&e, R_SR, R_SR, 12);
            g_sr_dirty = true;
            sext_memo_invalidate();
            emit_advance(&e, op_pc_bt, op_cyc_bt);
            u32 here_bt = e.len;
            i32 jo_bt = (i32)(here_bt - jbt_pos) - 4;
            u32 jw_bt = ((u32)((u32)jo_bt & 0x3FFFFu) << 6) | 0x06u;
            base[entry_off + jbt_pos    ] = (u8)jw_bt;
            base[entry_off + jbt_pos + 1] = (u8)(jw_bt >> 8);
            base[entry_off + jbt_pos + 2] = (u8)(jw_bt >> 16);

            inline_ops++; done = true;
        } else if (top == 0x0 && !((w >> 8) & 1) && szf == 0 && mode == 5
                   && ((w >> 9) & 7) == 0) {
            /* ORI.B #imm8,(d16,An) — boot-hot 0x002C at 408 K execs.
             * Length = 6 (opcode + imm word + d16). Cycles = 8. */
            int an = w & 7;
            u16 imm_word = mac_read16(cpu->mem, op_pc[i] + 2);
            u8 imm8 = (u8)imm_word;
            i16 d16 = (i16)mac_read16(cpu->mem, op_pc[i] + 4);

            emit_advance_flush(&e);
            emit_read_g(&e, &rc, G_A(an), 8);
            if (d16 >= -128 && d16 <= 127) {
                xt_addi(&e, 8, 8, d16);
            } else {
                emit_load_imm32(&e, 11, 12, (u32)(i32)d16);
                xt_add(&e, 8, 8, 11);
            }
            emit_l32r_at(&e, 9, lit_off[LITERAL_RAM_BOUNDS],
                         entry_off + e.len);
            xt_and(&e, 10, 8, 9);
            emit_cache_flush(&e, &rc);
            i32 op_pc_oo = 6, op_cyc_oo = 8;
            /* Custom helper bridge size: sr_flush (0/3) + s32i addr (3) +
             * movi imm (3) + s32i imm (3) + mov a2 (3) + l32r (3) +
             * callx0 (3) + sr_reload (3) + cache_reload (3*N). The
             * helper is m68k_jit_ori_b_mmio — it does just the byte
             * read/OR/write/CCR work, no PC or cycle advance (the JIT
             * supplies those via emit_advance below). */
            u32 mmio_bridge_size = 3*7;  /* mov, l32r, callx0, sr_reload, 3 setup */
            if (g_sr_dirty) mmio_bridge_size += 3;
            mmio_bridge_size += (u32)rc.active * 3;
            xt_beqz(&e, 10, (i32)(6u + mmio_bridge_size));
            /* --- Custom MMIO ORI.B helper bridge (replaces emit_helper_step). --- */
            sext_memo_invalidate();
            emit_sr_flush(&e);
            xt_s32i(&e, 8, R_CPU, OFF_JIT_ARG1);   /* arg1 = addr */
            xt_movi(&e, 9, imm8);
            xt_s32i(&e, 9, R_CPU, OFF_JIT_ARG2);   /* arg2 = imm */
            xt_mov (&e, R_ARG, R_CPU);             /* a2 = cpu */
            emit_l32r_at(&e, R_HELP, lit_off[HELPER_JIT_ORI_B_MMIO],
                         entry_off + e.len);
            xt_callx0(&e, R_HELP);
            emit_sr_reload(&e);
            emit_cache_reload(&e, &rc);
            /* --- end custom bridge --- */
            u32 joo_pos = e.len;
            xt_j(&e, 4);
            /* Fast path */
            emit_l32r_at(&e, 9, lit_off[ADDR_RAM_BASE],
                         entry_off + e.len);
            xt_add(&e, 9, 9, 8);                    /* a9 = ram + addr */
            xt_l8ui(&e, 10, 9, 0);                  /* a10 = byte */
            xt_movi(&e, 11, imm8);
            xt_or(&e, 10, 10, 11);
            xt_s8i(&e, 10, 9, 0);
            if (!flags_dead[i]) {
                xt_slli(&e, 11, 10, 24);            /* byte → bits 31..24 for N/Z derive */
                emit_logic_flags(&e, 11);
            }
            emit_advance(&e, op_pc_oo, op_cyc_oo);
            u32 here_oo = e.len;
            i32 jo_oo = (i32)(here_oo - joo_pos) - 4;
            u32 jw_oo = ((u32)((u32)jo_oo & 0x3FFFFu) << 6) | 0x06u;
            base[entry_off + joo_pos    ] = (u8)jw_oo;
            base[entry_off + joo_pos + 1] = (u8)(jw_oo >> 8);
            base[entry_off + joo_pos + 2] = (u8)(jw_oo >> 16);

            inline_ops++; done = true;
        }

        if (!done) {
            /* Helper fallback: m68k_step(cpu). a3 survives the call. */
            emit_helper_step(&e, lit_off[HELPER_M68K_STEP], entry_off, &rc);
            helper_ops++;
        }
    }

    /* 6. Epilogue: flush accumulated PC/cycles deltas, dirty cache slots
     * and R_SR back to cpu_state; restore the CALL0 return PC; JX back
     * to the dispatcher. */
    emit_advance_flush(&e);
    emit_cache_flush(&e, &rc);
    emit_sr_flush(&e);
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
