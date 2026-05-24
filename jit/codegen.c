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
#include <stddef.h>
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

/* M6.84 — set true by any function that emits a CALLX0 (which is the
 * only thing in the JIT's emitted code that clobbers a0 between the
 * prologue's `s32i a0, OFF_JITRETPC` and the epilogue's matching
 * `l32i a0, OFF_JITRETPC` + `jx a0`). When false at the epilogue, the
 * `l32i` can be skipped because a0 still holds the RET sentinel (= 0
 * from sim_init on host; = the chain-preserved value on ESP32). The
 * existing `helper_ops` counter only tracks the DEFAULT helper bridge
 * at the bottom of the dispatch chain; it misses the per-inline
 * bridges inside specific arms (MOVE.W (An), MOVEA.L (d16,An), etc.).
 * This flag catches all of them. */
static bool g_block_emitted_callx0;

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
    g_block_emitted_callx0 = true;
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
    g_block_emitted_callx0 = true;   /* M6.84 — see g_block_emitted_callx0 doc */
    /* M6.68 — the emit_sr_flush we're about to emit is in the SLOW-path
     * branch (caller put a beqz over us). At runtime it only runs if
     * the slow path is taken; the fast path skips it entirely. But the
     * compile-time side effect clears g_sr_dirty, which leaks into
     * subsequent code in this block. If R_SR was dirty on entry, we
     * must remember that so the *epilogue* knows to flush R_SR back to
     * cpu->sr (covering the fast-path case where the slow-path s16i
     * never ran). */
    bool was_sr_dirty = g_sr_dirty;
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
    /* M6.68 — restore the on-entry dirty state. The emit_sr_flush above
     * is conditional at runtime (slow path only), so its compile-time
     * effect of clearing g_sr_dirty would otherwise hide the fact that
     * the fast path may still have unflushed R_SR modifications.
     * Diagnosed via --diff-jit on speedo-bench.snap: SUBA.L bridge +
     * ADDQ.W + RTS, where RTS's fast-path inline (SP in RAM) doesn't
     * run the slow-path s16i. */
    if (was_sr_dirty) g_sr_dirty = true;
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
    g_block_emitted_callx0 = true;   /* M6.84 — see g_block_emitted_callx0 doc */
    sext_memo_invalidate();
    /* M6.69 — same compile-time-leak class as M6.68. This helper is
     * called from the slow-path branch of a beqz/bnez; its emit_sr_flush
     * is inside that branch but clears g_sr_dirty in the compile-time
     * state. Snapshot + restore so the fast-path case (where the s16i
     * never runs) still has the epilogue flush R_SR. */
    bool was_sr_dirty = g_sr_dirty;
    emit_sr_flush(e);
    xt_s32i(e, a8_arg1_reg, R_CPU, OFF_JIT_ARG1);
    xt_movi(e, 9, imm2);
    xt_s32i(e, 9, R_CPU, OFF_JIT_ARG2);
    xt_mov (e, R_ARG, R_CPU);
    emit_l32r_at(e, R_HELP, helper_lit_off, entry_off + e->len);
    xt_callx0(e, R_HELP);
    emit_sr_reload(e);
    emit_cache_reload(e, rc);
    if (was_sr_dirty) g_sr_dirty = true;
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
 * X preserved. `vreg` is not clobbered. R_SR is updated in place.
 *
 * IMPORTANT (M6.77 fix): the body uses a10 and a11 as scratch. The
 * VERY FIRST emitted instruction overwrites a10. If the caller passes
 * vreg ∈ {10, 11}, the subsequent `extui` and `bnez` would read the
 * clobbered value instead of the intended source — yielding the wrong
 * N (=1, always) and the wrong Z (=0, always). Existing callers in
 * M6.62 / M6.73 etc. routinely pass vreg=10 (the same scratch the
 * load-/.L-assemble code paths land the value in) and have been
 * silently relying on the lazy-CC pass marking flags_dead=true for the
 * common-case follow-on, which made the bug invisible.
 *
 * Caught on M6.77's TST.B (xxx).W inline whose follow-on is a Bcc that
 * really does consume Z — the bench got stuck at PC=0x4028F0 (the BEQ
 * after TST.B at 0x4028DE) because Z was wrong. Fix: shadow vreg into
 * a9 before clobbering a10/a11. */
static void emit_logic_flags(xt_emit *e, u8 vreg) {
    if (vreg == 10 || vreg == 11) {
        xt_mov(e, 9, vreg);   /* a9 is not touched by the body */
        vreg = 9;
    }
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

/* M6.85b — Fusion: LEA (d8,An,Xn.W),Am  +  ADDA.W Xn,Am   (Xn shared).
 *
 * Bench's 0x03DF4E-50:
 *   LEA (2,A4,D6.W),A2 ; ADDA.W D6,A2     →     A2 = A4 + 2·sext_w(D6) + 2
 *
 * Standard emit:
 *   LEA mode 6 fast path = 4 ops (a8=A4; a13=sext D6; a8+=a13; a8+=d8)
 *   + ADDA.W = 1 op (xt_add Am, Am, a13)
 *   = 5 LX7 (assuming sext memo'd from the upstream ADDA)
 *
 * Fused emit:
 *   xt_addx2 dst, a13, A4_src           ; dst = 2·sext + A4
 *   xt_addi  dst, dst, d8                ; dst += d8        (skip if d8==0)
 *   = 2 LX7 (1 if d8==0)
 *
 * Saves 3 LX7 per occurrence. Bench at 0x03DF4E (~405 K iters)
 * = ~1.2 M LX7 / 20 M cyc ≈ 5 % on bench. No CCR concerns. */
static void emit_lea_adda_fused(xt_emit *e, int dst_am, int lea_an, int idx_reg,
                                bool idx_is_an, int d8, regcache *rc) {
    int g_idx = idx_is_an ? G_A(idx_reg) : G_D(idx_reg);

    /* Ensure sext_w(g_idx) is live in a13. Reuse if memoized. */
    if (!g_sext_valid || g_sext_src_reg != g_idx) {
        emit_read_g(e, rc, g_idx, 13);
        xt_slli(e, 13, 13, 16);
        xt_srai(e, 13, 13, 16);
        g_sext_src_reg = g_idx;
        g_sext_valid = true;
    }

    /* Get LEA's An base into a host reg. */
    int xt_an = cache_lookup(rc, G_A(lea_an));
    u8 src_reg;
    if (xt_an >= 0) {
        src_reg = (u8)xt_an;
    } else {
        xt_l32i(e, 8, R_CPU, OFF_A(lea_an));
        src_reg = 8;
    }

    int xt_dst = cache_lookup(rc, G_A(dst_am));
    if (xt_dst >= 0) {
        u8 d = (u8)xt_dst;
        xt_addx2(e, d, 13, src_reg);                 /* d = 2·sext + An */
        if (d8 != 0) xt_addi(e, d, d, d8);
        for (int i = 0; i < rc->active; i++)
            if (rc->guest[i] == (u8)G_A(dst_am)) { rc->dirty |= (u16)(1u << i); break; }
    } else {
        xt_addx2(e, 9, 13, src_reg);
        if (d8 != 0) xt_addi(e, 9, 9, d8);
        xt_s32i(e, 9, R_CPU, OFF_A(dst_am));
    }

    /* PC delta: 4 (LEA mode 6) + 2 (ADDA.W) = 6.
     * Cycles:   8 (LEA flat) + 12 (ADDA.W)  = 20. */
    emit_advance(e, 6, 20);
}

/* M6.85 — Fusion: MOVEA.L An|Dn,Am  +  ADDA.W Dx|Ax,Am  [+ ADDA.W Dx|Ax,Am].
 *
 * Bench's hot block 0x03DF40 (405 K hits / 20 M cyc) is dominated by
 * three patterns of this shape:
 *   MOVEA.L A4,A0 ; ADDA.W D6,A0                      → A0 = A4 + sext_w(D6)
 *   MOVEA.L A4,A3 ; ADDA.W D6,A3 ; ADDA.W D6,A3       → A3 = A4 + 2*sext_w(D6)
 *
 * Standard emit: 2-3 LX7 ops (xt_mov + xt_add[+xt_add], assuming both
 * dst and src cached and the sext-memo is already live). Fused emit
 * collapses the MOVEA's xt_mov into the ADDA's xt_add: a single
 *   xt_add Am, src_movea, a13                   (double-fuse)
 *   xt_addx2 Am, a13, src_movea                 (triple-fuse, 2*sext + src)
 *
 * Cycle accounting absorbs both/all ops in the fused emit: MOVEA.L = 8,
 * ADDA.W = 12 each, so double = 20 c / 4 bytes, triple = 32 c / 6 bytes.
 *
 * Wins (cached dst+src, typical case):
 *   Double: 1 LX7 saved (xt_mov gone)
 *   Triple: 2 LX7 saved (xt_mov + 1 xt_add gone)
 *
 * On bench's 0x03DF40 (one double + one triple per iter, 405K iters):
 *   1.2 M LX7 / 20 M cyc = ~0.06 lx7_per_cyc improvement (~5 %).
 *
 * The fusion is no-op-equivalent for CCR (neither MOVEA nor ADDA touch
 * flags) and the cache dirty-marking still happens. */
static void emit_movea_adda_fused(xt_emit *e, int an, int movea_src, bool movea_src_is_an,
                                  int adda_src, bool adda_src_is_an, bool triple,
                                  regcache *rc) {
    int g_msrc = movea_src_is_an ? G_A(movea_src) : G_D(movea_src);
    int g_asrc = adda_src_is_an  ? G_A(adda_src)  : G_D(adda_src);
    int g_dst  = G_A(an);

    /* Ensure sext_w(g_asrc) is live in a13. Reuse if memoized. */
    if (!g_sext_valid || g_sext_src_reg != g_asrc) {
        emit_read_g(e, rc, g_asrc, 13);
        xt_slli(e, 13, 13, 16);
        xt_srai(e, 13, 13, 16);
        g_sext_src_reg = g_asrc;
        g_sext_valid = true;
    }

    int xt_msrc = cache_lookup(rc, g_msrc);
    int xt_dst  = cache_lookup(rc, g_dst);

    /* Get MOVEA src into a host reg `src_reg`. If cached, use the slot
     * directly; else load into a8. */
    u8 src_reg;
    if (xt_msrc >= 0) {
        src_reg = (u8)xt_msrc;
    } else {
        if (g_msrc < 8) xt_l32i(e, 8, R_CPU, OFF_D(g_msrc));
        else            xt_l32i(e, 8, R_CPU, OFF_A(g_msrc - 8));
        src_reg = 8;
    }

    if (xt_dst >= 0) {
        u8 d = (u8)xt_dst;
        /* Triple-fuse only if requested AND xt_addx2 won't clobber the
         * sext source (it can't — d is the dst, a13 is the as, src_reg
         * is at). */
        if (triple) xt_addx2(e, d, 13, src_reg);   /* d = 2*sext + src */
        else        xt_add  (e, d, src_reg, 13);   /* d = src + sext   */
        for (int i = 0; i < rc->active; i++)
            if (rc->guest[i] == (u8)g_dst) { rc->dirty |= (u16)(1u << i); break; }
    } else {
        if (triple) xt_addx2(e, 9, 13, src_reg);
        else        xt_add  (e, 9, src_reg, 13);
        xt_s32i(e, 9, R_CPU, OFF_A(an));
    }

    /* PC delta: 2 (MOVEA) + 2 (ADDA) [+ 2 (ADDA)].
     * Cycles:   8 + 12 [+ 12]. */
    if (triple) emit_advance(e, 6, 32);
    else        emit_advance(e, 4, 20);
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
    /* ADDA.W cycles = 8 (handler) + 4 (m68k_step base) = 12. */
    emit_advance(e, 4, 12);
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
static void emit_add_l_dd(xt_emit *e, int dn, int dm, bool src_is_an,
                          bool skip_flags, regcache *rc) {
    int g_src = src_is_an ? G_A(dm) : G_D(dm);
    int xt_dm = cache_lookup(rc, g_src);
    int xt_dn = cache_lookup(rc, G_D(dn));
    if (xt_dm >= 0 && xt_dn >= 0 && (src_is_an || dn != dm)) {
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
    emit_read_g(e, rc, g_src, 8);       /* s */
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

/* SUB.L Dm/Am,Dn — d[dn] -= src.L, full CCR (mirrors emit_add_l_dd).
 * M6.104 — src_is_an supports SUB.L An,Dn (bench-warm 0x948a). */
static void emit_sub_l_dd(xt_emit *e, int dn, int dm, bool src_is_an,
                          bool skip_flags, regcache *rc) {
    int g_src = src_is_an ? G_A(dm) : G_D(dm);
    int xt_dm = cache_lookup(rc, g_src);
    int xt_dn = cache_lookup(rc, G_D(dn));
    if (xt_dm >= 0 && xt_dn >= 0 && (src_is_an || dn != dm)) {
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
    emit_read_g(e, rc, g_src, 8);
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

/* M6.100 — AND.B/W / OR.B/W / EOR.B/W Dm,Dn register form:
 *   d[dst].size = d[dst].size <op> d[src].size
 *   d[dst]'s bits above size are preserved
 *   N = bit (size-1) of result, Z = (result == 0), V=C=0, X preserved
 * `kind`: 0 = OR, 1 = AND, 2 = EOR.
 * `size_bits`: 8 (.B) or 16 (.W).
 * Boot-warm 0xC242 (AND.W D2,D1) at 525 helpers / 100 M cyc + variants. */
static void emit_logic_bw_dd_kind(xt_emit *e, int ra, int rb, int dst,
                                  int kind, int size_bits,
                                  bool skip_flags, regcache *rc) {
    u8 src_reg = emit_read_g_in(e, rc, G_D(ra), 8);
    int dst_xt = cache_lookup(rc, G_D(dst));
    u8 dst_slot = (dst_xt >= 0) ? (u8)dst_xt : 9;
    if (dst_xt < 0) emit_read_g(e, rc, G_D(dst), 9);

    /* Extract size halves, combine, merge into dst. */
    xt_extui(e, 10, src_reg, 0, size_bits - 1);
    xt_extui(e, 11, dst_slot, 0, size_bits - 1);
    if      (kind == 0) xt_or (e, 10, 10, 11);
    else if (kind == 1) xt_and(e, 10, 10, 11);
    else                xt_xor(e, 10, 10, 11);  /* a10 = result.size (low bits) */

    /* Merge: clear low size_bits of dst, OR in result. */
    xt_srli(e, 11, dst_slot, size_bits);
    xt_slli(e, 11, 11, size_bits);
    u8 out_reg = (dst_xt >= 0) ? (u8)dst_xt : 9;
    xt_or  (e, out_reg, 11, 10);

    if (dst_xt >= 0) {
        for (int i = 0; i < rc->active; i++)
            if (rc->guest[i] == (u8)G_D(dst)) { rc->dirty |= (u16)(1u << i); break; }
    } else {
        emit_write_g(e, rc, G_D(dst), 9);
    }
    if (!skip_flags) {
        /* Shift result.size to bit 31 so emit_logic_flags reads bit
         * (size-1) as N. */
        xt_slli(e, 8, 10, 32 - size_bits);
        emit_logic_flags(e, 8);
    }
    emit_advance(e, 2, 4 + (size_bits == 8 ? 4 : 4));  /* base 4 + handler 4 */
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
 * when no consumer reads the flags before the next setter.
 *
 * M6.87 — when `skip_flags` is true (bench's 0x03DF58/5E ADDQ.W #1,D6
 * followed by CMP.W → Bcc, the flags get overwritten before any reader),
 * emit the simpler "compute low 16, OR with high 16" form: 6 LX7 ops
 * between read and write, vs 8 in the shifted-form path needed for
 * `emit_addsub_flags_long`. With both bench blocks firing ADDQ.W #1,D6
 * (~404 K hits / 20 M cyc), saves ~4 LX7 per call cached = ~1.6 M LX7
 * total ≈ 6 % bench. */
static void emit_addq_w_dn(xt_emit *e, int dn, int imm, bool is_sub, bool skip_flags, regcache *rc) {
    if (skip_flags) {
        /* Lean value-only path. Uses emit_read_g_in/cache_lookup so the
         * cached fast path emits zero load/store overhead. */
        u8 src_reg = emit_read_g_in(e, rc, G_D(dn), 8);
        int xt_dst = cache_lookup(rc, G_D(dn));
        u8 dst_reg = (xt_dst >= 0) ? (u8)xt_dst : 8;

        xt_extui(e, 9, src_reg, 0, 15);                /* a9 = Dn.low_16 */
        xt_addi (e, 9, 9, is_sub ? -(i32)imm : imm);   /* a9 = .low ± imm */
        xt_extui(e, 9, 9, 0, 15);                       /* mask back to 16 b */
        xt_extui(e, 10, src_reg, 16, 15);              /* a10 = Dn.high_16 */
        xt_slli (e, 10, 10, 16);                        /* a10 = high << 16 */
        xt_or   (e, dst_reg, 10, 9);                    /* combined */

        if (xt_dst >= 0) {
            for (int i = 0; i < rc->active; i++)
                if (rc->guest[i] == (u8)G_D(dn)) { rc->dirty |= (u16)(1u << i); break; }
        } else {
            xt_s32i(e, dst_reg, R_CPU, OFF_D(dn));
        }
        emit_advance(e, 2, 8);
        return;
    }

    /* Full-CCR path: shifted-to-high-16 form so emit_addsub_flags_long
     * can read s, d, r directly from a8, a9, a10. */
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
    emit_addsub_flags_long(e, is_sub, false);
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
        /* JMP/JSR/RTS/RTE/RTR/NOP/STOP/RTD — control flow, treat as
         * consumer-only for our purposes (they keep the CCR you set
         * just before, for the caller / fall-through to read). */
        if (w == 0x4E75 || w == 0x4E71 || w == 0x4E73 || w == 0x4E77 ||
            w == 0x4E72 ||                            /* STOP #imm16 */
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
        /* M6.116 — CCR-neutral subroutine plumbing. Each falls to m68k_step
         * (no inline arm), but none reads or writes CCR — so classify as
         * transparent (0u) lets prior-op flag emits be marked dead when
         * the next SET-class consumer is past this op.
         *
         *   LINK An,#d16  (0x4E50-0x4E57) — subroutine stack-frame setup
         *   UNLK An       (0x4E58-0x4E5F) — subroutine stack-frame teardown
         *   PEA <ea>      (0x4840-0x487F, mode 2..7) — push ea onto SP
         *   MOVE USP      (0x4E60-0x4E6F) — privileged An↔USP move
         *   RTD #d16      (0x4E74)        — return + adjust SP
         *
         * Conservative on a few: SWAP/EXT (mode 0) overlaps PEA's hi mask,
         * so check mode > 1 explicitly. */
        if ((w & 0xFFF8) == 0x4E50) return 0u;   /* LINK An,#d16 */
        if ((w & 0xFFF8) == 0x4E58) return 0u;   /* UNLK An */
        if ((w & 0xFFC0) == 0x4840 && ((w >> 3) & 7) > 1) return 0u; /* PEA */
        if ((w & 0xFFF0) == 0x4E60) return 0u;   /* MOVE An,USP / USP,An */
        /* RTD #d16 — like RTS, control flow that preserves CCR for caller. */
        if (w == 0x4E74) return CONS;
        return SET | CONS;
    }
    case 0x5:                              /* ADDQ/SUBQ/Scc/DBcc */
        if (szf == 3) return CONS;         /* Scc/DBcc */
        if (mode == 1) return 0u;          /* to An — no flags */
        return SET;
    case 0x6:                              /* Bcc.S / BRA.S / BSR.S */
        return (((w >> 8) & 0xF) > 1) ? CONS : 0u;
    case 0x7: return SET;                  /* MOVEQ */
    case 0x8: case 0xC: {
        /* OR / DIVU/DIVS / AND / MULU/MULS / ABCD / SBCD / EXG.
         * ABCD/SBCD consume X. Encoding: 1c0r_rrr_1_0000_a_sss with
         * bits 8 = 1 AND bits 7-4 = 0000. (top=0x8 → SBCD, 0xC → ABCD.) */
        if (((w >> 8) & 1) && ((w >> 4) & 0xF) == 0) {
            return SET | CONS;             /* ABCD / SBCD */
        }
        return SET;                        /* OR / DIVU/DIVS / AND / MULU/MULS / EXG */
    }
    case 0xB: case 0xF:
        return SET;                        /* CMP / EOR / fp */
    case 0xE: {
        /* Shifts. ROXR/ROXL consume X — must be SET|CONS so prior op's
         * flag emit (which sets X) isn't marked dead.
         *
         * Encoding split:
         *   register-form  (szf != 3): type in bits 4-3 (10 = ROX)
         *   memory-form    (szf == 3, mode-EA): type in bits 11-9 (010 = ROX) */
        int szf_local = (w >> 6) & 3;
        if (szf_local == 3) {
            int t = (w >> 9) & 7;
            if (t == 2) return SET | CONS; /* ROXR/ROXL <ea> */
        } else {
            int t = (w >> 3) & 3;
            if (t == 2) return SET | CONS; /* ROXR/ROXL Dn */
        }
        return SET;                        /* ASR/ASL/LSR/LSL/ROR/ROL */
    }
    case 0x9: case 0xD:                    /* SUB/ADD family — SUBA/ADDA no flags */
        if (szf == 3) return 0u;
        /* M6.114 — refine: only ADDX/SUBX consume X (and Z, sticky).
         * Plain ADD/SUB just SET — they don't read CCR.
         *
         * ADDX/SUBX shape: bit 8 = 1 (to_ea direction) AND mode in {0, 1}
         * (mode 0 = Dn,Dn form; mode 1 = -(An),-(An) form). Plain ADD/SUB
         * with bit 8 = 1 has mode in {2..7} (memory destination).
         *
         * Previously all top=9/D returned SET|CONS — overly conservative,
         * marking the prior op's flag-emit as live even though plain ADD
         * doesn't read CCR. Refinement turns SET|CONS into SET for ~all
         * arithmetic, letting more prior-op flag emits be marked dead. */
        {
            bool addx_to_ea = (w >> 8) & 1;
            int  addx_mode  = (w >> 3) & 7;
            if (addx_to_ea && (addx_mode == 0 || addx_mode == 1)) {
                return SET | CONS;          /* ADDX/SUBX */
            }
            return SET;                     /* plain ADD/SUB */
        }
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
        if ((last_op >> 12) == 0x6) {        /* BRA / Bcc / BSR family */
            int cc = (last_op >> 8) & 0xF;
            i32 disp = (i8)(last_op & 0xFF);
            if (disp != 0 && disp != -1) {  /* .S form */
                u32 pc_const;
                if (cc == 0 || cc == 1) {
                    /* BRA.S / BSR.S — both unconditional, literal is the
                     * taken target. (M6.83 added BSR.S to this branch.) */
                    pc_const = op_pc[n_ops - 1] + 2 + (u32)disp;
                } else {
                    /* Bcc.S: literal is the fall-through PC. */
                    pc_const = op_pc[n_ops - 1] + 2;
                }
                *(u32 *)(base + lit_off[LITERAL_BCC_PC]) = pc_const;
                g_pc_lit_val = pc_const;
                g_pc_lit_valid = true;
            } else if (disp == 0 && cc == 1) {
                /* M6.105 — BSR.W disp16: target = source_pc + 2 +
                 * sext16(disp16). Unconditional; literal is the taken
                 * target. Bench has 0x6100 (BSR.W) at 117 hits / 20 M cyc;
                 * boot has BSR.W variants in subroutine-heavy code paths. */
                u16 ext = mac_read16(cpu->mem, op_pc[n_ops - 1] + 2);
                u32 pc_const = op_pc[n_ops - 1] + 2 + (u32)(i32)(i16)ext;
                *(u32 *)(base + lit_off[LITERAL_BCC_PC]) = pc_const;
                g_pc_lit_val = pc_const;
                g_pc_lit_valid = true;
            } else if (disp == 0 && cc != 1) {
                /* M6.106 — BRA.W / Bcc.W disp16. For cc=0 (BRA): literal
                 * is the taken target. For cc>=2 (Bcc): literal is the
                 * fall-through PC (= op_pc + 4 since Bcc.W is 4 bytes).
                 * Mirrors the .S logic above but with the 4-byte op length. */
                u32 pc_const;
                if (cc == 0) {
                    u16 ext = mac_read16(cpu->mem, op_pc[n_ops - 1] + 2);
                    pc_const = op_pc[n_ops - 1] + 2 + (u32)(i32)(i16)ext;
                } else {
                    pc_const = op_pc[n_ops - 1] + 4;
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
        } else if (last_op == 0x4EF9) {
            /* JMP (xxx).L — store the absolute 32-bit target in the literal
             * pool so the inline arm can load it with a 1-op l32r. */
            u32 pc_const = mac_read32(cpu->mem, op_pc[n_ops - 1] + 2);
            *(u32 *)(base + lit_off[LITERAL_BCC_PC]) = pc_const;
            g_pc_lit_val = pc_const;
            g_pc_lit_valid = true;
        } else if (last_op == 0x4EBA) {
            /* JSR (d16,PC) — store target PC (= source_pc + 2 + sext_d16)
             * for the M6.78 inline arm to load with a 1-op l32r. */
            u16 ext = mac_read16(cpu->mem, op_pc[n_ops - 1] + 2);
            u32 pc_const = op_pc[n_ops - 1] + 2 + (u32)(i32)(i16)ext;
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
             * "this register won't be cached", which is safe.
             *
             * M6.89 attempt: tried a per-top-precise destination counter
             * (CMP.W's true Dn dst, ADDQ's true dst at sm/sr, LEA's An
             * dst at dr). Cache match rate IMPROVED (3.3 % → 4.8 %),
             * BUT bench regressed +1.6 % — the "more accurate" picks
             * evicted A4 / A3 (heavily read by MOVEA/ADDA emits in
             * bench's 0x03DF40 hot block) in favour of D5 / D6 (counted
             * twice each by the fix). The runtime savings from extra
             * cache matches (~16 K LX7) were eclipsed by ~400 K LX7 of
             * uncached load/stores. Conservative under-counting at the
             * destination side is empirically the better default. */
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
    g_block_emitted_callx0 = false;   /* M6.84 — epilogue uses this to gate
                                       * the a0 reload before `jx a0`. */
    emit_l32r_at(&e, R_CPU, lit_off[ADDR_CPU_BASE], entry_off + e.len);
    xt_s32i(&e, 0, R_CPU, OFF_JITRETPC);
    /* M6.82 — chain_entry skip target. Both the l32r above and the s32i
     * above are redundant on chain-transition entries: R_CPU is a3
     * (callee-saved across the JX), and the chain epilogue reloads a0
     * from OFF_JITRETPC before the JX so the s32i would just write the
     * same value back. Dispatcher uses this offset when chain hits but
     * cache_sig doesn't match (body_addr is unsafe but the first two
     * ops aren't needed). */
    u32 chain_entry_off = entry_off + e.len;
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

    /* M6.62: record the offset where the body begins — chain-skip target
     * when the predecessor's cache + SR state already matches what this
     * block's prologue would have set up. */
    u32 body_off = entry_off + e.len;

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
        } else if (w == 0x4EF9) {
            /* JMP (xxx).L — absolute long jump. Block terminator.
             * Target is a compile-time constant; load via the literal pool
             * (1 op) when set up by the M6.30 PC-literal logic, else fall
             * back to emit_load_imm32 (10 ops). */
            u32 target = mac_read32(cpu->mem, op_pc[i] + 2);
            emit_advance_flush(&e);
            if (g_pc_lit_valid && target == g_pc_lit_val) {
                emit_l32r_at(&e, 8, g_pc_lit_off, g_pc_lit_entry_off + e.len);
            } else {
                emit_load_imm32(&e, 8, 9, target);
            }
            xt_s32i(&e, 8, R_CPU, OFF_PC);
            emit_advance(&e, 0, 12);   /* JMP .L cycles = 8 + 4 = 12 */
            sext_memo_invalidate();
            inline_ops++; done = true;
        } else if (w == 0x4E75) {
            /* RTS — pop 32-bit PC from stack, set cpu->pc, SP += 4.
             * Block terminator. Bench-hot (7K). Cycles = 16 = 4 + 12. */
            emit_advance_flush(&e);
            emit_read_g(&e, &rc, G_A(7), 8);              /* a8 = SP */
            emit_l32r_at(&e, 9, lit_off[LITERAL_RAM_BOUNDS],
                         entry_off + e.len);
            xt_and  (&e, 10, 8, 9);
            emit_cache_flush(&e, &rc);
            i32 op_pc_rts = 0;        /* m68k_step's RTS handler sets pc directly */
            i32 op_cyc_rts = 16;      /* full m68k_step adds: 4 base + 12 handler */
            xt_beqz (&e, 10, (i32)(6u + helper_step_after_flush_undo_size(&rc, op_pc_rts, op_cyc_rts)));
            emit_helper_step_after_flush_undo(&e, lit_off[HELPER_M68K_STEP],
                                              entry_off, &rc, op_pc_rts, op_cyc_rts);
            u32 jrts_pos = e.len;
            xt_j    (&e, 4);
            /* Fast path: read 4 BE bytes from mem[SP] into a10, write to
             * cpu->pc, post-increment SP by 4. Cycles handled by the
             * emit_advance(0, 16) below + block-end flush. */
            emit_l32r_at(&e, 9, lit_off[ADDR_RAM_BASE],
                         entry_off + e.len);
            xt_add  (&e, 9, 9, 8);                        /* a9 = ram + SP */
            xt_l8ui (&e, 10, 9, 0);
            xt_l8ui (&e, 11, 9, 1);
            xt_slli (&e, 10, 10, 24);
            xt_slli (&e, 11, 11, 16);
            xt_or   (&e, 10, 10, 11);
            xt_l8ui (&e, 11, 9, 2);
            xt_l8ui (&e, 12, 9, 3);
            xt_slli (&e, 11, 11, 8);
            xt_or   (&e, 10, 10, 11);
            xt_or   (&e, 10, 10, 12);                     /* a10 = popped pc */
            xt_s32i (&e, 10, R_CPU, OFF_PC);
            xt_addi (&e, 8, 8, 4);
            emit_write_g(&e, &rc, G_A(7), 8);
            emit_advance(&e, 0, 16);

            u32 here_rts = e.len;
            i32 jo_rts = (i32)(here_rts - jrts_pos) - 4;
            u32 jw_rts = ((u32)((u32)jo_rts & 0x3FFFFu) << 6) | 0x06u;
            base[entry_off + jrts_pos    ] = (u8)jw_rts;
            base[entry_off + jrts_pos + 1] = (u8)(jw_rts >> 8);
            base[entry_off + jrts_pos + 2] = (u8)(jw_rts >> 16);

            sext_memo_invalidate();
            inline_ops++; done = true;
        } else if (w == 0x4EBA) {
            /* JSR (d16,PC) — push return address, jump to PC + 2 + sext_d16.
             * Block terminator. Bench-hot at 2563 hits/20 M cyc on the
             * post-M6.77 corrected path (M6.78).
             *
             * Compile-time constants:
             *   target_pc = source_pc + 2 + sext16(d16)   (stashed in
             *                                              LITERAL_BCC_PC
             *                                              by the block
             *                                              setup pre-pass)
             *   return_pc = source_pc + 4                 (computed at
             *                                              runtime as
             *                                              cpu->pc + 4,
             *                                              since the
             *                                              emit_advance_flush
             *                                              just landed
             *                                              cpu->pc on
             *                                              source_pc) */
            emit_advance_flush(&e);            /* cpu->pc = source_pc */
            emit_read_g(&e, &rc, G_A(7), 8);   /* a8 = SP */
            xt_addi(&e, 8, 8, -4);              /* a8 = new SP */
            emit_l32r_at(&e, 9, lit_off[LITERAL_RAM_BOUNDS],
                         entry_off + e.len);
            xt_and  (&e, 10, 8, 9);
            emit_cache_flush(&e, &rc);
            i32 op_pc_jsr = 0;        /* m68k_step sets cpu->pc directly */
            i32 op_cyc_jsr = 20;      /* full m68k_step: base 4 + handler 16 */
            xt_beqz (&e, 10, (i32)(6u + helper_step_after_flush_undo_size(&rc, op_pc_jsr, op_cyc_jsr)));
            emit_helper_step_after_flush_undo(&e, lit_off[HELPER_M68K_STEP],
                                              entry_off, &rc, op_pc_jsr, op_cyc_jsr);
            u32 jjsr_pos = e.len;
            xt_j    (&e, 4);
            /* Fast path: write return_pc (= cpu->pc + 4) as 4 BE bytes to
             * [ram_base + new SP]; commit new SP; load target_pc into
             * cpu->pc. */
            xt_l32i (&e, 10, R_CPU, OFF_PC);
            xt_addi (&e, 10, 10, 4);            /* a10 = return_pc */
            emit_l32r_at(&e, 9, lit_off[ADDR_RAM_BASE],
                         entry_off + e.len);
            xt_add  (&e, 9, 9, 8);
            xt_extui(&e, 11, 10, 24, 7); xt_s8i(&e, 11, 9, 0);
            xt_extui(&e, 11, 10, 16, 7); xt_s8i(&e, 11, 9, 1);
            xt_extui(&e, 11, 10,  8, 7); xt_s8i(&e, 11, 9, 2);
            xt_extui(&e, 11, 10,  0, 7); xt_s8i(&e, 11, 9, 3);
            emit_write_g(&e, &rc, G_A(7), 8);
            /* Set cpu->pc = target. LITERAL_BCC_PC holds target_pc. */
            if (g_pc_lit_valid) {
                emit_l32r_at(&e, 10, g_pc_lit_off,
                             g_pc_lit_entry_off + e.len);
            } else {
                u16 ext = mac_read16(cpu->mem, op_pc[i] + 2);
                u32 target_pc = op_pc[i] + 2 + (u32)(i32)(i16)ext;
                emit_load_imm32(&e, 10, 11, target_pc);
            }
            xt_s32i(&e, 10, R_CPU, OFF_PC);
            emit_advance(&e, op_pc_jsr, op_cyc_jsr);

            u32 here_jsr = e.len;
            i32 jo_jsr = (i32)(here_jsr - jjsr_pos) - 4;
            u32 jw_jsr = ((u32)((u32)jo_jsr & 0x3FFFFu) << 6) | 0x06u;
            base[entry_off + jjsr_pos    ] = (u8)jw_jsr;
            base[entry_off + jjsr_pos + 1] = (u8)(jw_jsr >> 8);
            base[entry_off + jjsr_pos + 2] = (u8)(jw_jsr >> 16);

            sext_memo_invalidate();
            inline_ops++; done = true;
        } else if ((w & 0xFFF8) == 0x4EA8) {
            /* JSR (d16,An) — push return address, jump to An + sext_d16.
             * Block terminator. Bench-warm at 1003 hits/20M cyc (M6.79).
             *
             * Sibling of M6.78's JSR (d16,PC) but the target is computed
             * at runtime from An (it's not a compile-time constant), so
             * we stash it in a15 BEFORE the bounds check (a15 survives
             * emit_cache_flush and the helper bridge because cache slots
             * are a4..a7 and the bridge clobbers a8..a13 + R_HELP=a13 +
             * a2 but not a15 — and the fast path uses a15 immediately
             * after the beqz takes, so the runtime never reads a15 along
             * the helper-path branch). */
            int src_an = w & 7;
            u16 ext = mac_read16(cpu->mem, op_pc[i] + 2);
            i32 d16 = (i16)ext;

            emit_advance_flush(&e);          /* cpu->pc = source_pc */

            /* Compute target = source_An + sext_d16 → a15. */
            emit_read_g(&e, &rc, G_A(src_an), 15);
            if (d16 >= -128 && d16 <= 127) {
                xt_addi(&e, 15, 15, d16);
            } else {
                emit_load_imm(&e, 11, 12, (u32)d16);
                xt_add (&e, 15, 15, 11);
            }

            /* Push return PC: same shape as JSR (d16,PC). */
            emit_read_g(&e, &rc, G_A(7), 8);
            xt_addi(&e, 8, 8, -4);            /* a8 = new SP */
            emit_l32r_at(&e, 9, lit_off[LITERAL_RAM_BOUNDS],
                         entry_off + e.len);
            xt_and  (&e, 10, 8, 9);
            emit_cache_flush(&e, &rc);
            i32 op_pc_jsra = 0;
            i32 op_cyc_jsra = 20;            /* base 4 + handler 16 */
            xt_beqz (&e, 10, (i32)(6u + helper_step_after_flush_undo_size(&rc, op_pc_jsra, op_cyc_jsra)));
            emit_helper_step_after_flush_undo(&e, lit_off[HELPER_M68K_STEP],
                                              entry_off, &rc, op_pc_jsra, op_cyc_jsra);
            u32 jjsra_pos = e.len;
            xt_j    (&e, 4);
            /* Fast path: write return_pc to mem[new SP], then commit new
             * SP and set cpu->pc = target (in a15). */
            xt_l32i (&e, 10, R_CPU, OFF_PC);
            xt_addi (&e, 10, 10, 4);          /* a10 = return_pc */
            emit_l32r_at(&e, 9, lit_off[ADDR_RAM_BASE],
                         entry_off + e.len);
            xt_add  (&e, 9, 9, 8);
            xt_extui(&e, 11, 10, 24, 7); xt_s8i(&e, 11, 9, 0);
            xt_extui(&e, 11, 10, 16, 7); xt_s8i(&e, 11, 9, 1);
            xt_extui(&e, 11, 10,  8, 7); xt_s8i(&e, 11, 9, 2);
            xt_extui(&e, 11, 10,  0, 7); xt_s8i(&e, 11, 9, 3);
            emit_write_g(&e, &rc, G_A(7), 8);
            xt_s32i(&e, 15, R_CPU, OFF_PC);
            emit_advance(&e, op_pc_jsra, op_cyc_jsra);

            u32 here_jsra = e.len;
            i32 jo_jsra = (i32)(here_jsra - jjsra_pos) - 4;
            u32 jw_jsra = ((u32)((u32)jo_jsra & 0x3FFFFu) << 6) | 0x06u;
            base[entry_off + jjsra_pos    ] = (u8)jw_jsra;
            base[entry_off + jjsra_pos + 1] = (u8)(jw_jsra >> 8);
            base[entry_off + jjsra_pos + 2] = (u8)(jw_jsra >> 16);

            sext_memo_invalidate();
            inline_ops++; done = true;
        } else if (top == 0xD && szf == 2 && !((w >> 8) & 1) && (mode == 0 || mode == 1)) {
            /* ADD.L Dm/Am,Dn — M6.104 extends to An source. */
            emit_add_l_dd(&e, (w >> 9) & 7, w & 7, mode == 1, flags_dead[i], &rc);
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
        } else if (top == 0x2 && ((w >> 3) & 7) == 7 && (w & 7) == 4
                   && (((w >> 6) & 7) == 0 || ((w >> 6) & 7) == 1)) {
            /* MOVE.L / MOVEA.L #imm32, Dn / An  (src = immediate, dst_mode
             * 0 or 1 only — other dst_modes (e.g. -(An), (d16,An)) get
             * their own arms later in this chain). The previous form of
             * this `else if` matched ANY dst_mode and left `done = false`
             * for dst_modes ∉ {0,1}, suppressing dispatch of those later
             * arms — caught when the M6.78 MOVE.L #imm32,-(An) arm never
             * fired against the bench's 0x2F3C hot spot. */
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
            /* M6.85 — Peek for fusion: MOVEA.L <Dm|Am>,Am followed by
             * ADDA.W <Dx|Ax>,Am (same Am). If the op after the ADDA is
             * ALSO ADDA.W with the same Ax,Am, triple-fuse via xt_addx2.
             * No CCR concerns (neither op touches flags). */
            int am = (w >> 9) & 7;
            bool fused = false;
            if (i + 1 < n_ops) {
                u16 nw = op_word[i + 1];
                if (((nw >> 12) & 0xF) == 0xD
                    && ((nw >> 6) & 7) == 3
                    && ((nw >> 3) & 7) <= 1
                    && ((nw >> 9) & 7) == am) {
                    int adda_src     = nw & 7;
                    bool adda_src_an = ((nw >> 3) & 7) == 1;
                    bool triple = false;
                    if (i + 2 < n_ops) {
                        u16 nw2 = op_word[i + 2];
                        if (((nw2 >> 12) & 0xF) == 0xD
                            && ((nw2 >> 6) & 7) == 3
                            && ((nw2 >> 3) & 7) <= 1
                            && ((nw2 >> 9) & 7) == am
                            && (nw2 & 7) == adda_src
                            && (((nw2 >> 3) & 7) == 1) == adda_src_an) {
                            triple = true;
                        }
                    }
                    emit_movea_adda_fused(&e, am, w & 7, mode == 1,
                                          adda_src, adda_src_an, triple, &rc);
                    i += triple ? 2 : 1;   /* skip absorbed ADDA(s) */
                    fused = true;
                }
            }
            if (!fused) {
                emit_movea_l_reg(&e, am, w & 7, mode == 1, &rc);
            }
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
            /* MOVE.W Dm,Dn — aggressive (held back under --diff-jit).
             *
             * M6.88 — when flags are dead (lazy-CC marked the next op as a
             * setter that overwrites all CCR), use a lean 4-op cached
             * path that combines source low 16 with destination high 16
             * directly into the dst slot. Bench's 0x03DF58 has
             * `MOVE.W D5,D4` followed by `MOVE.W (A2),(A3)` (setter) →
             * flags dead → ~212 K hits / 20 M cyc × 3 LX7 saved ≈ 3 %
             * bench. */
            int dn = (w >> 9) & 7;
            int dm = w & 7;
            if (flags_dead[i]) {
                u8 src_reg = emit_read_g_in(&e, &rc, G_D(dm), 9);
                int xt_dst = cache_lookup(&rc, G_D(dn));
                if (xt_dst >= 0) {
                    u8 d = (u8)xt_dst;
                    xt_extui(&e, 12, src_reg, 0, 15);   /* a12 = dm.low_16 */
                    xt_extui(&e, 11, d, 16, 15);         /* a11 = dn.high_16 */
                    xt_slli (&e, 11, 11, 16);            /* a11 = high << 16 */
                    xt_or   (&e, d, 11, 12);              /* dn = combined */
                    for (int j = 0; j < rc.active; j++)
                        if (rc.guest[j] == (u8)G_D(dn)) { rc.dirty |= (u16)(1u << j); break; }
                } else {
                    /* Uncached dst: load + merge + store. */
                    xt_l32i(&e, 11, R_CPU, OFF_D(dn));
                    xt_extui(&e, 12, src_reg, 0, 15);
                    xt_extui(&e, 11, 11, 16, 15);
                    xt_slli (&e, 11, 11, 16);
                    xt_or   (&e, 11, 11, 12);
                    xt_s32i (&e, 11, R_CPU, OFF_D(dn));
                }
                emit_advance(&e, 2, 8);
                inline_ops++; done = true;
            } else {
                emit_read_g(&e, &rc, G_D(dm), 9);
                emit_read_g(&e, &rc, G_D(dn), 11);
                xt_srli (&e, 11, 11, 16);
                xt_slli (&e, 11, 11, 16);
                xt_extui(&e, 12, 9, 0, 15);
                xt_or   (&e, 11, 11, 12);
                emit_write_g(&e, &rc, G_D(dn), 11);
                xt_slli (&e, 8, 9, 16);
                emit_logic_flags(&e, 8);
                emit_advance(&e, 2, 8);
                inline_ops++; done = true;
            }
        } else if (top == 0x3 && ((w >> 6) & 7) == 0 && mode == 7 && (w & 7) == 4) {
            /* MOVE.W #imm16,Dn — bench-warm 0x303C+reg<<9 at ~1000 helpers
             * in 20M cyc (M6.73). Replace low 16 of Dn with imm16, full
             * MOVE-family flags (N from imm.W, Z if imm==0, V=C=0, X kept).
             *
             * IMPORTANT: xt_movi only encodes -2048..2047. For arbitrary
             * imm16 we MUST go via emit_load_imm (which falls back to a
             * multi-op build for values out of the 12-bit range). The
             * silent wrap of xt_movi for |imm| > 2047 corrupted Dn during
             * boot and was caught by boot-path divergence vs the interp. */
            int dn = (w >> 9) & 7;
            u16 imm = mac_read16(cpu->mem, op_pc[i] + 2);
            emit_read_g(&e, &rc, G_D(dn), 11);     /* a11 = old Dn */
            xt_srli (&e, 11, 11, 16);
            xt_slli (&e, 11, 11, 16);               /* keep high 16, clear low */
            emit_load_imm(&e, 10, 12, (u32)imm);    /* a10 = imm (zero-ext) */
            xt_or   (&e, 11, 11, 10);
            emit_write_g(&e, &rc, G_D(dn), 11);
            if (!flags_dead[i]) {
                xt_slli (&e, 8, 10, 16);            /* sign of .W in bit 31 */
                emit_logic_flags(&e, 8);
            }
            emit_advance(&e, 4, 8);                 /* length 4 (op + imm16) */
            inline_ops++; done = true;
        } else if (top == 0x1 && ((w >> 6) & 7) == 0 && mode == 0) {
            /* M6.101 — MOVE.B Dm,Dn (register-to-register byte move).
             * Bench-warm 0x1003 (MOVE.B D3,D0) at 192 helpers / 20 M cyc.
             * Replace Dn[7:0] with Dm[7:0]; preserve Dn[31:8].
             * MOVE-family flags from byte sign.
             *
             * Length 2, cycles 8 (m68k_step base 4 + MOVE handler `+= 4`).
             * Initial emit had cycles=4 which caused massive JIT/interp
             * cycle drift on boot 100M → 1.4 M extra bogus-PC helpers
             * from the M6.66 divergence path running differently. The
             * diff_jit_bench_lockstep at 11 K cycles passed because the
             * drift hadn't accumulated enough yet; boot 100M caught it. */
            int dn = (w >> 9) & 7;
            int dm = w & 7;
            u8 src_reg = emit_read_g_in(&e, &rc, G_D(dm), 9);
            int dst_xt = cache_lookup(&rc, G_D(dn));
            u8 dst_slot = (dst_xt >= 0) ? (u8)dst_xt : 9;
            if (dst_xt < 0) emit_read_g(&e, &rc, G_D(dn), 9);

            xt_extui(&e, 12, src_reg, 0, 7);           /* a12 = Dm.B */
            xt_srli (&e, 11, dst_slot, 8);
            xt_slli (&e, 11, 11, 8);                   /* a11 = Dn.high_24 << 8 */
            u8 out_reg = (dst_xt >= 0) ? (u8)dst_xt : 9;
            xt_or   (&e, out_reg, 11, 12);

            if (dst_xt >= 0) {
                for (int s = 0; s < rc.active; s++)
                    if (rc.guest[s] == (u8)G_D(dn)) { rc.dirty |= (u16)(1u << s); break; }
            } else {
                emit_write_g(&e, &rc, G_D(dn), 9);
            }
            if (!flags_dead[i]) {
                /* Shift byte to bit 31 for emit_logic_flags. */
                xt_slli(&e, 8, 12, 24);
                emit_logic_flags(&e, 8);
            }
            emit_advance(&e, 2, 8);
            inline_ops++; done = true;
        } else if (top == 0x3 && ((w >> 6) & 7) == 1 && mode == 0) {
            /* MOVEA.W Dn,An — bench-warm 0x3040+ at ~1000 helpers in 20M
             * (M6.73). Sign-extend Dn's low .W to 32 and store in An;
             * MOVEA never touches flags. */
            int an = (w >> 9) & 7;
            int dn = w & 7;
            emit_read_g(&e, &rc, G_D(dn), 8);       /* a8 = Dn */
            xt_slli (&e, 9, 8, 16);                  /* a9 = Dn.W << 16 */
            xt_srai (&e, 9, 9, 16);                  /* a9 = sign-ext .W */
            emit_write_g(&e, &rc, G_A(an), 9);
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
            if (flags_dead[i]) {
                /* M6.90 — when flags are dead, the byte order in the
                 * intermediate register doesn't matter; only the bytes
                 * landing at mem[dst+0..1] in BE order matters. Xtensa's
                 * `l16ui at,as,imm` reads bytes p[0]+(p[1]<<8) (little-
                 * endian in the register), and `s16i at,as,imm` writes
                 * back byte-swapped. The net effect is a 2-byte copy
                 * preserving BE order on the destination — saves 2 LX7
                 * ops per execution (4 byte ops → 1 l16ui + 1 s16i).
                 *
                 * Bench's 0x3692 (MOVE.W (A2),(A3)) at ~60 K runtime
                 * iters / 20 M cyc × 2 LX7 saved ≈ 0.5 % bench.
                 *
                 * Alignment: LITERAL_RAM_BOUNDS = `~(ram_size-1) | 1`
                 * fails the AND fast-path for any odd address, so the
                 * fast path only runs when both src and dst are 2-byte
                 * aligned (l16ui/s16i requirement). */
                xt_l16ui(&e, 10, 8, 0);
                xt_s16i (&e, 10, 9, 0);
            } else {
                xt_l8ui (&e, 10, 8, 0);              /* read high */
                xt_l8ui (&e, 12, 8, 1);              /* read low  */
                xt_s8i  (&e, 10, 9, 0);              /* write high */
                xt_s8i  (&e, 12, 9, 1);              /* write low  */
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
        } else if (top == 0x2 && ((w >> 6) & 7) == 0 && mode == 2) {
            /* M6.101 — MOVE.L (An),Dn — bench-warm 0x2014 (MOVE.L (A4),D0)
             * at 157 helpers / 20 M cyc. Sibling of MOVE.L (An)+,Dn but
             * without the post-increment. Length 2, cycles 8.
             *
             * Cycle count: m68k_step base 4 + handler 4 = 8 (matches
             * MOVE-family). Initial mis-set to 4 would cause boot 100M
             * drift past the M6.66 boundary — see
             * memory/move-cycle-drift-gotcha.md. */
            int dn = (w >> 9) & 7;
            int an = w & 7;

            emit_advance_flush(&e);
            emit_read_g(&e, &rc, G_A(an), 8);
            emit_l32r_at(&e, 9, lit_off[LITERAL_RAM_BOUNDS],
                         entry_off + e.len);
            xt_and  (&e, 10, 8, 9);
            emit_cache_flush(&e, &rc);
            i32 op_pc_lan = 2, op_cyc_lan = 8;
            xt_beqz (&e, 10, (i32)(6u + helper_step_after_flush_undo_size(&rc, op_pc_lan, op_cyc_lan)));
            emit_helper_step_after_flush_undo(&e, lit_off[HELPER_M68K_STEP],
                                              entry_off, &rc, op_pc_lan, op_cyc_lan);
            u32 jlan_pos = e.len;
            xt_j    (&e, 4);
            /* Fast path: read 4 BE bytes into a10 (.L value). */
            emit_l32r_at(&e, 9, lit_off[ADDR_RAM_BASE],
                         entry_off + e.len);
            xt_add  (&e, 9, 9, 8);
            xt_l8ui (&e, 11, 9, 0);
            xt_l8ui (&e, 12, 9, 1);
            xt_slli (&e, 10, 11, 24);
            xt_slli (&e, 12, 12, 16);
            xt_or   (&e, 10, 10, 12);
            xt_l8ui (&e, 11, 9, 2);
            xt_l8ui (&e, 12, 9, 3);
            xt_slli (&e, 11, 11, 8);
            xt_or   (&e, 10, 10, 11);
            xt_or   (&e, 10, 10, 12);                /* a10 = .L value */
            emit_write_g(&e, &rc, G_D(dn), 10);
            if (!flags_dead[i]) emit_logic_flags(&e, 10);
            emit_advance(&e, op_pc_lan, op_cyc_lan);

            u32 here_lan = e.len;
            i32 jo_lan = (i32)(here_lan - jlan_pos) - 4;
            u32 jw_lan = ((u32)((u32)jo_lan & 0x3FFFFu) << 6) | 0x06u;
            base[entry_off + jlan_pos    ] = (u8)jw_lan;
            base[entry_off + jlan_pos + 1] = (u8)(jw_lan >> 8);
            base[entry_off + jlan_pos + 2] = (u8)(jw_lan >> 16);

            inline_ops++; done = true;
        } else if (top == 0x2 && ((w >> 6) & 7) == 0 && mode == 7 && (w & 7) == 0) {
            /* M6.108 — MOVE.L (xxx).W,Dn — bench-hot 0x2438 at 21 K helpers /
             * 100 M cyc (in the bench's post-cycle-11898 path). The .W
             * absolute address is a signed 16-bit ext word, so its 24-bit
             * range is [0, 0x7FFF] (low RAM globals) or [0xFF8000,
             * 0xFFFFFE] (high MMIO). Sibling of M6.77's TST.B (xxx).W —
             * static RAM check at compile time, helper bridge for MMIO.
             *
             * For Mac Plus init code, low-RAM globals at 0x000xxx are
             * heavily used; bench's 100 M path hits these via this
             * encoding. Length 4 (op + abs.W ext), cycles 8. */
            int dn = (w >> 9) & 7;
            u16 ext = mac_read16(cpu->mem, op_pc[i] + 2);
            u32 abs_addr = (u32)(i32)(i16)ext;
            abs_addr &= 0xFFFFFFu;

            u32 ram_size = cpu->mem ? cpu->mem->ram_size : 0;
            bool overlay = cpu->mem ? cpu->mem->overlay : true;
            bool ram_pow2 = ram_size > 0 && (ram_size & (ram_size - 1)) == 0;
            bool addr_in_ram = !overlay && ram_pow2
                               && abs_addr < ram_size
                               && (abs_addr & 1) == 0;  /* .L needs even */

            if (addr_in_ram) {
                emit_advance_flush(&e);
                emit_l32r_at(&e, 9, lit_off[ADDR_RAM_BASE],
                             entry_off + e.len);
                if ((i32)abs_addr >= -128 && (i32)abs_addr <= 127) {
                    xt_addi(&e, 9, 9, (i32)abs_addr);
                } else if ((i32)abs_addr >= -2048 && (i32)abs_addr <= 2047) {
                    xt_movi(&e, 10, (i32)abs_addr);
                    xt_add (&e, 9, 9, 10);
                } else {
                    emit_load_imm(&e, 10, 11, abs_addr);
                    xt_add (&e, 9, 9, 10);
                }
                /* Read 4 BE bytes into a10 (.L value). */
                xt_l8ui (&e, 11, 9, 0);
                xt_l8ui (&e, 12, 9, 1);
                xt_slli (&e, 10, 11, 24);
                xt_slli (&e, 12, 12, 16);
                xt_or   (&e, 10, 10, 12);
                xt_l8ui (&e, 11, 9, 2);
                xt_l8ui (&e, 12, 9, 3);
                xt_slli (&e, 11, 11, 8);
                xt_or   (&e, 10, 10, 11);
                xt_or   (&e, 10, 10, 12);          /* a10 = .L */
                emit_write_g(&e, &rc, G_D(dn), 10);
                if (!flags_dead[i]) emit_logic_flags(&e, 10);
                emit_advance(&e, 4, 8);
                inline_ops++; done = true;
            }
            /* else: fall through to helper. */
        } else if (top == 0x2 && ((w >> 6) & 7) == 1 && mode == 7 && (w & 7) == 0) {
            /* M6.108 — MOVEA.L (xxx).W,Am — sibling of MOVE.L (xxx).W,Dn
             * but writes to An (MOVEA never touches CCR). Bench-warm
             * 0x2078 (MOVEA.L (xxx).W,A0) appears at 84 helpers in 20 M cyc
             * and likely more in the 100 M post-divergence path. */
            int an = (w >> 9) & 7;
            u16 ext = mac_read16(cpu->mem, op_pc[i] + 2);
            u32 abs_addr = (u32)(i32)(i16)ext;
            abs_addr &= 0xFFFFFFu;

            u32 ram_size = cpu->mem ? cpu->mem->ram_size : 0;
            bool overlay = cpu->mem ? cpu->mem->overlay : true;
            bool ram_pow2 = ram_size > 0 && (ram_size & (ram_size - 1)) == 0;
            bool addr_in_ram = !overlay && ram_pow2
                               && abs_addr < ram_size
                               && (abs_addr & 1) == 0;

            if (addr_in_ram) {
                emit_advance_flush(&e);
                emit_l32r_at(&e, 9, lit_off[ADDR_RAM_BASE],
                             entry_off + e.len);
                if ((i32)abs_addr >= -128 && (i32)abs_addr <= 127) {
                    xt_addi(&e, 9, 9, (i32)abs_addr);
                } else if ((i32)abs_addr >= -2048 && (i32)abs_addr <= 2047) {
                    xt_movi(&e, 10, (i32)abs_addr);
                    xt_add (&e, 9, 9, 10);
                } else {
                    emit_load_imm(&e, 10, 11, abs_addr);
                    xt_add (&e, 9, 9, 10);
                }
                xt_l8ui (&e, 11, 9, 0);
                xt_l8ui (&e, 12, 9, 1);
                xt_slli (&e, 10, 11, 24);
                xt_slli (&e, 12, 12, 16);
                xt_or   (&e, 10, 10, 12);
                xt_l8ui (&e, 11, 9, 2);
                xt_l8ui (&e, 12, 9, 3);
                xt_slli (&e, 11, 11, 8);
                xt_or   (&e, 10, 10, 11);
                xt_or   (&e, 10, 10, 12);
                emit_write_g(&e, &rc, G_A(an), 10);
                /* MOVEA — no flags. */
                emit_advance(&e, 4, 8);
                inline_ops++; done = true;
            }
            /* else: fall through to helper. */
        } else if (top == 0x2 && ((w >> 6) & 7) == 1 && mode == 2) {
            /* M6.103 — MOVEA.L (An),Am — boot-warm 0x2050 (MOVEA.L (A0),A0)
             * at 390 helpers / 100 M cyc. Sibling of M6.101's
             * MOVE.L (An),Dn but writes 32-bit result to Am (MOVEA never
             * touches CCR). Same RAM-only bounds; ROM-source variant
             * could be added later via the M6.76 unified-bounds shape.
             *
             * Length 2, cycles 8. Same-An edge case (e.g. 0x2050 reads
             * from A0 and writes to A0): the read happens before the
             * write, so order is preserved naturally — emit_read_g
             * reads A0 into a8, then we read 4 bytes from (a8), then
             * emit_write_g writes the result back to A0's cache slot. */
            int dst_am = (w >> 9) & 7;
            int src_an = w & 7;

            emit_advance_flush(&e);
            emit_read_g(&e, &rc, G_A(src_an), 8);
            emit_l32r_at(&e, 9, lit_off[LITERAL_RAM_BOUNDS],
                         entry_off + e.len);
            xt_and  (&e, 10, 8, 9);
            emit_cache_flush(&e, &rc);
            i32 op_pc_maan = 2, op_cyc_maan = 8;
            xt_beqz (&e, 10, (i32)(6u + helper_step_after_flush_undo_size(&rc, op_pc_maan, op_cyc_maan)));
            emit_helper_step_after_flush_undo(&e, lit_off[HELPER_M68K_STEP],
                                              entry_off, &rc, op_pc_maan, op_cyc_maan);
            u32 jmaan_pos = e.len;
            xt_j    (&e, 4);

            /* Fast path: read 4 BE bytes into a10 (.L value). */
            emit_l32r_at(&e, 9, lit_off[ADDR_RAM_BASE],
                         entry_off + e.len);
            xt_add  (&e, 9, 9, 8);
            xt_l8ui (&e, 11, 9, 0);
            xt_l8ui (&e, 12, 9, 1);
            xt_slli (&e, 10, 11, 24);
            xt_slli (&e, 12, 12, 16);
            xt_or   (&e, 10, 10, 12);
            xt_l8ui (&e, 11, 9, 2);
            xt_l8ui (&e, 12, 9, 3);
            xt_slli (&e, 11, 11, 8);
            xt_or   (&e, 10, 10, 11);
            xt_or   (&e, 10, 10, 12);                    /* a10 = .L value */
            emit_write_g(&e, &rc, G_A(dst_am), 10);
            /* MOVEA — no flags. */
            emit_advance(&e, op_pc_maan, op_cyc_maan);

            u32 here_maan = e.len;
            i32 jo_maan = (i32)(here_maan - jmaan_pos) - 4;
            u32 jw_maan = ((u32)((u32)jo_maan & 0x3FFFFu) << 6) | 0x06u;
            base[entry_off + jmaan_pos    ] = (u8)jw_maan;
            base[entry_off + jmaan_pos + 1] = (u8)(jw_maan >> 8);
            base[entry_off + jmaan_pos + 2] = (u8)(jw_maan >> 16);

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
        } else if (top == 0x2 && ((w >> 6) & 7) == 1 && mode == 3) {
            /* MOVEA.L (An)+,Am — bench-hot 0x28D8 (MOVEA.L (A0)+,A4) at
             * ~71 K helpers in 20M cyc (M6.74). Mirrors MOVE.L (An)+,Dn
             * but writes 32-bit result to Am (no sign-extend needed for
             * long), no flag computation (MOVEA never touches CCR). */
            int dst_am = (w >> 9) & 7;        /* dst Am */
            int src_an = w & 7;               /* src An (post-incr) */

            emit_advance_flush(&e);
            emit_read_g(&e, &rc, G_A(src_an), 8);
            emit_l32r_at(&e, 9, lit_off[LITERAL_RAM_BOUNDS],
                         entry_off + e.len);
            xt_and  (&e, 10, 8, 9);
            emit_cache_flush(&e, &rc);
            i32 op_pc_mAl = 2, op_cyc_mAl = 8;
            xt_beqz (&e, 10, (i32)(6u + helper_step_after_flush_undo_size(&rc, op_pc_mAl, op_cyc_mAl)));
            emit_helper_step_after_flush_undo(&e, lit_off[HELPER_M68K_STEP],
                                              entry_off, &rc, op_pc_mAl, op_cyc_mAl);
            u32 jmAl_pos = e.len;
            xt_j    (&e, 4);
            /* Fast path: 4 BE byte loads. */
            emit_l32r_at(&e, 9, lit_off[ADDR_RAM_BASE],
                         entry_off + e.len);
            xt_add  (&e, 9, 9, 8);
            xt_l8ui (&e, 11, 9, 0);
            xt_l8ui (&e, 12, 9, 1);
            xt_slli (&e, 10, 11, 24);
            xt_slli (&e, 12, 12, 16);
            xt_or   (&e, 10, 10, 12);
            xt_l8ui (&e, 11, 9, 2);
            xt_l8ui (&e, 12, 9, 3);
            xt_slli (&e, 11, 11, 8);
            xt_or   (&e, 10, 10, 11);
            xt_or   (&e, 10, 10, 12);                /* a10 = .L */
            emit_write_g(&e, &rc, G_A(dst_am), 10);
            /* Post-increment An by 4 (the src). */
            xt_addi (&e, 8, 8, 4);
            emit_write_g(&e, &rc, G_A(src_an), 8);
            /* MOVEA — no flags. */
            emit_advance(&e, op_pc_mAl, op_cyc_mAl);

            u32 mAl_here = e.len;
            i32 jo_mAl = (i32)(mAl_here - jmAl_pos) - 4;
            u32 jwAl = ((u32)((u32)jo_mAl & 0x3FFFFu) << 6) | 0x06u;
            base[entry_off + jmAl_pos    ] = (u8)jwAl;
            base[entry_off + jmAl_pos + 1] = (u8)(jwAl >> 8);
            base[entry_off + jmAl_pos + 2] = (u8)(jwAl >> 16);

            inline_ops++; done = true;
        } else if (top == 0x2 && ((w >> 6) & 7) == 4 && (mode == 0 || mode == 1)) {
            /* MOVE.L Dn|Am,-(An) — pre-decrement push pattern (boot 0x24C3
             * MOVE.L D3,-(A2) ~5 K, bench 0x2F08 MOVE.L A0,-(SP) ~2 K).
             * Mirrors MOVE.L Dn,(An)+ but with An decremented by 4. */
            int an = (w >> 9) & 7;
            int dm = w & 7;
            int g_src = (mode == 1) ? G_A(dm) : G_D(dm);

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
            emit_read_g(&e, &rc, g_src, 10);
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
        } else if (top == 0x2 && ((w >> 6) & 7) == 4 && mode == 7 && (w & 7) == 4) {
            /* MOVE.L #imm32,-(An) — immediate push. Bench-hot 0x2F3C
             * (MOVE.L #imm32,-(SP)) at 1008 hits/20 M cyc on the
             * corrected M6.77 path. Mirrors the MOVE.L Dn|Am,-(An) arm
             * above but with a compile-time-known src value: the
             * four byte stores load the byte constants directly via
             * `xt_movi` (range -2048..+2047 — every byte 0..255 fits)
             * instead of `extui`-ing them out of a runtime register.
             * Also folds the MOVE-family CCR update into compile-time:
             * the imm's N/Z bits are constant, so we emit the SR
             * mask + OR with a constant set-bits value (4 ops vs
             * emit_logic_flags' 8 ops on a runtime value). */
            int an = (w >> 9) & 7;
            u32 imm = mac_read32(cpu->mem, op_pc[i] + 2);

            emit_advance_flush(&e);
            emit_read_g(&e, &rc, G_A(an), 8);
            xt_addi(&e, 8, 8, -4);                    /* a8 = new An */
            emit_l32r_at(&e, 9, lit_off[LITERAL_RAM_BOUNDS],
                         entry_off + e.len);
            xt_and  (&e, 10, 8, 9);
            emit_cache_flush(&e, &rc);
            i32 op_pc_pi = 6, op_cyc_pi = 8;     /* len 6 (op + imm32), 8 cyc */
            xt_beqz (&e, 10, (i32)(6u + helper_step_after_flush_undo_size(&rc, op_pc_pi, op_cyc_pi)));
            emit_helper_step_after_flush_undo(&e, lit_off[HELPER_M68K_STEP],
                                              entry_off, &rc, op_pc_pi, op_cyc_pi);
            u32 jpi_pos = e.len;
            xt_j    (&e, 4);

            /* Fast path: write imm32 as 4 BE bytes to [ram_base + new An]. */
            emit_l32r_at(&e, 9, lit_off[ADDR_RAM_BASE],
                         entry_off + e.len);
            xt_add  (&e, 9, 9, 8);
            xt_movi (&e, 11, (i32)((imm >> 24) & 0xFF)); xt_s8i(&e, 11, 9, 0);
            xt_movi (&e, 11, (i32)((imm >> 16) & 0xFF)); xt_s8i(&e, 11, 9, 1);
            xt_movi (&e, 11, (i32)((imm >>  8) & 0xFF)); xt_s8i(&e, 11, 9, 2);
            xt_movi (&e, 11, (i32)((imm >>  0) & 0xFF)); xt_s8i(&e, 11, 9, 3);
            emit_write_g(&e, &rc, G_A(an), 8);

            if (!flags_dead[i]) {
                /* Compile-time MOVE-family flags from the imm. Clear low
                 * 4 bits of SR (preserve X via -16 mask), OR in the
                 * statically-known N/Z bits. */
                xt_movi (&e, 12, -16);
                xt_and  (&e, R_SR, R_SR, 12);
                u32 set_bits = 0;
                if (imm == 0)          set_bits = 0x04;  /* Z */
                else if ((i32)imm < 0) set_bits = 0x08;  /* N */
                if (set_bits) {
                    xt_movi(&e, 12, (i32)set_bits);
                    xt_or  (&e, R_SR, R_SR, 12);
                }
                g_sr_dirty = true;
                sext_memo_invalidate();
            }
            emit_advance(&e, op_pc_pi, op_cyc_pi);

            u32 here_pi = e.len;
            i32 jo_pi = (i32)(here_pi - jpi_pos) - 4;
            u32 jw_pi = ((u32)((u32)jo_pi & 0x3FFFFu) << 6) | 0x06u;
            base[entry_off + jpi_pos    ] = (u8)jw_pi;
            base[entry_off + jpi_pos + 1] = (u8)(jw_pi >> 8);
            base[entry_off + jpi_pos + 2] = (u8)(jw_pi >> 16);

            inline_ops++; done = true;
        } else if (top == 0x2 && ((w >> 6) & 7) == 2 && (mode == 0 || mode == 1)) {
            /* M6.93 — MOVE.L Dn|Am,(An) — boot's 0x228a (MOVE.L A2,(A1))
             * at 626 helpers, plus various other A/D source variants.
             * Sibling of the M6.91-era MOVE.L Dn|Am,(An)+ arm just below
             * but without the post-increment. Same RAM-only byte-bounds
             * (writes to ROM not supported); 4 byte stores BE; MOVE-family
             * flags from the .L value. Length 2, cycles 8. */
            int an = (w >> 9) & 7;            /* dst An (no post-incr) */
            int dm = w & 7;                   /* src reg */
            int g_src = (mode == 1) ? G_A(dm) : G_D(dm);

            emit_advance_flush(&e);
            emit_read_g(&e, &rc, G_A(an), 8);
            emit_l32r_at(&e, 9, lit_off[LITERAL_RAM_BOUNDS],
                         entry_off + e.len);
            xt_and  (&e, 10, 8, 9);
            emit_cache_flush(&e, &rc);
            i32 op_pc_lp2 = 2, op_cyc_lp2 = 8;
            xt_beqz (&e, 10, (i32)(6u + helper_step_after_flush_undo_size(&rc, op_pc_lp2, op_cyc_lp2)));
            emit_helper_step_after_flush_undo(&e, lit_off[HELPER_M68K_STEP],
                                              entry_off, &rc, op_pc_lp2, op_cyc_lp2);
            u32 jlp2_pos = e.len;
            xt_j    (&e, 4);
            /* Fast path: load src reg, write 4 BE bytes. */
            emit_read_g(&e, &rc, g_src, 10);
            emit_l32r_at(&e, 9, lit_off[ADDR_RAM_BASE],
                         entry_off + e.len);
            xt_add  (&e, 9, 9, 8);
            xt_extui(&e, 11, 10, 24, 7); xt_s8i(&e, 11, 9, 0);
            xt_extui(&e, 11, 10, 16, 7); xt_s8i(&e, 11, 9, 1);
            xt_extui(&e, 11, 10,  8, 7); xt_s8i(&e, 11, 9, 2);
            xt_extui(&e, 11, 10,  0, 7); xt_s8i(&e, 11, 9, 3);
            if (!flags_dead[i]) emit_logic_flags(&e, 10);
            emit_advance(&e, op_pc_lp2, op_cyc_lp2);

            u32 here_lp2 = e.len;
            i32 jo_lp2 = (i32)(here_lp2 - jlp2_pos) - 4;
            u32 jw_lp2 = ((u32)((u32)jo_lp2 & 0x3FFFFu) << 6) | 0x06u;
            base[entry_off + jlp2_pos    ] = (u8)jw_lp2;
            base[entry_off + jlp2_pos + 1] = (u8)(jw_lp2 >> 8);
            base[entry_off + jlp2_pos + 2] = (u8)(jw_lp2 >> 16);

            inline_ops++; done = true;
        } else if (top == 0x2 && ((w >> 6) & 7) == 3 && (mode == 0 || mode == 1)) {
            /* MOVE.L Dn|Am,(An)+ — boot-hot 0x20C1 at 262 K execs / 60 M cycles.
             * Bounds check the An address; on fast path do 4 byte writes
             * (BE), post-increment An by 4, emit logic flags if needed. */
            int an = (w >> 9) & 7;        /* dst An (post-incr) */
            int dm = w & 7;               /* src reg */
            int g_src = (mode == 1) ? G_A(dm) : G_D(dm);

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
            /* Fast path: load src reg, write 4 BE bytes, post-incr An. */
            emit_read_g(&e, &rc, g_src, 10);             /* a10 = Dn or Am */
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
        } else if (top == 0x2 && ((w >> 6) & 7) == 3 && mode == 3) {
            /* MOVE.L (An)+,(Am)+ — mem-to-mem long copy. Bench-hot
             * 0x28D8 (MOVE.L (A0)+,(A4)+) at ~71 K helpers in 20 M cyc;
             * 0x22D8 (MOVE.L (A0)+,(A1)+) at ~13 K (M6.76).
             *
             * Bit-field decode (per memory/triple-differential.md
             * post-M6.75 lesson):
             *   0x28D8 = 0010_1000_1101_1000
             *            top=2  reg=4 m=3   m=3 reg=0
             *                        dst   src
             *   → top=0x2 (.L MOVE), dst_mode=3 ((An)+), dst_reg=A4,
             *     src_mode=3 ((An)+), src_reg=A0.
             *
             * Source admits RAM OR ROM (Speedometer reads pointer
             * tables in ROM at PC ≈ 0x408???). Destination admits RAM
             * only. Bounds-check both upfront; both-OK → inline copy.
             *
             * Same-An edge case (e.g. MOVE.L (A0)+,(A0)+): src ea_decode
             * captures orig and increments An; dst ea_decode captures
             * the now-incremented An and increments again. So the write
             * happens at orig+4 and An ends at orig+8. We mirror this
             * by resynching the dst-An register (a15) to the post-incr
             * src An (a8) when src_an == dst_am. */
            int dst_am = (w >> 9) & 7;
            int src_an = w & 7;

            emit_advance_flush(&e);
            emit_read_g(&e, &rc, G_A(src_an), 8);    /* a8  = src An */
            emit_read_g(&e, &rc, G_A(dst_am), 15);   /* a15 = dst An (pre-incr) */

            /* Source bounds: a12 == 0 iff src in RAM OR ROM. */
            emit_l32r_at(&e, 9, lit_off[LITERAL_RAM_BOUNDS],
                         entry_off + e.len);
            xt_and  (&e, 10, 8, 9);                   /* a10 = src & RAM_MASK */
            emit_l32r_at(&e, 9, lit_off[LITERAL_ROM_BOUNDS],
                         entry_off + e.len);
            xt_and  (&e, 11, 8, 9);
            emit_l32r_at(&e, 9, lit_off[LITERAL_ROM_BASE],
                         entry_off + e.len);
            xt_xor  (&e, 11, 11, 9);                  /* a11 = (src & ROM_M) ^ ROM_BASE */
            xt_and  (&e, 12, 10, 11);                 /* a12 = 0 iff src RAM-or-ROM */

            /* Dest bounds (RAM only) folded into a12. */
            emit_l32r_at(&e, 9, lit_off[LITERAL_RAM_BOUNDS],
                         entry_off + e.len);
            xt_and  (&e, 9, 15, 9);                   /* a9 = dst & RAM_MASK */
            xt_or   (&e, 12, 12, 9);                  /* a12 == 0 iff both pass */

            emit_cache_flush(&e, &rc);
            i32 op_pc_mm = 2, op_cyc_mm = 8;
            xt_beqz (&e, 12, (i32)(6u + helper_step_after_flush_undo_size(&rc, op_pc_mm, op_cyc_mm)));
            emit_helper_step_after_flush_undo(&e, lit_off[HELPER_M68K_STEP],
                                              entry_off, &rc, op_pc_mm, op_cyc_mm);
            u32 jmm_pos = e.len;
            xt_j    (&e, 4);

            /* === Fast path === */
            /* Pick src host base: RAM_BASE if a10==0 (in RAM), else ROM. */
            emit_l32r_at(&e, 9, lit_off[ADDR_RAM_BASE],
                         entry_off + e.len);
            xt_beqz (&e, 10, 6);                     /* rel=6 skips one l32r */
            emit_l32r_at(&e, 9, lit_off[ADDR_ROM_HOST_BASE],
                         entry_off + e.len);
            xt_add  (&e, 9, 9, 8);                   /* a9 = src host ptr */

            /* Read 4 BE bytes from a9 → a10 (.L value). */
            xt_l8ui (&e, 11, 9, 0);
            xt_l8ui (&e, 12, 9, 1);
            xt_slli (&e, 10, 11, 24);
            xt_slli (&e, 12, 12, 16);
            xt_or   (&e, 10, 10, 12);
            xt_l8ui (&e, 11, 9, 2);
            xt_l8ui (&e, 12, 9, 3);
            xt_slli (&e, 11, 11, 8);
            xt_or   (&e, 10, 10, 11);
            xt_or   (&e, 10, 10, 12);                /* a10 = .L value */

            /* Post-incr src An — commit BEFORE the dst An read sync, in
             * line with the 68k same-An semantic (dst captures the
             * already-incremented An). */
            xt_addi (&e, 8, 8, 4);
            emit_write_g(&e, &rc, G_A(src_an), 8);
            if (src_an == dst_am) {
                xt_mov(&e, 15, 8);  /* a15 = orig+4 (the new "current" An) */
            }

            /* Dest host ptr: always RAM (dst bounds enforced above). */
            emit_l32r_at(&e, 9, lit_off[ADDR_RAM_BASE],
                         entry_off + e.len);
            xt_add  (&e, 9, 9, 15);                  /* a9 = dst host ptr */

            /* Write 4 BE bytes from a10 → [a9]. */
            xt_extui(&e, 11, 10, 24, 7); xt_s8i(&e, 11, 9, 0);
            xt_extui(&e, 11, 10, 16, 7); xt_s8i(&e, 11, 9, 1);
            xt_extui(&e, 11, 10,  8, 7); xt_s8i(&e, 11, 9, 2);
            xt_extui(&e, 11, 10,  0, 7); xt_s8i(&e, 11, 9, 3);

            /* Post-incr dst An. */
            xt_addi (&e, 15, 15, 4);
            emit_write_g(&e, &rc, G_A(dst_am), 15);

            /* MOVE-family flags from the .L value. */
            if (!flags_dead[i]) emit_logic_flags(&e, 10);

            emit_advance(&e, op_pc_mm, op_cyc_mm);

            u32 mm_here = e.len;
            i32 jo_mm = (i32)(mm_here - jmm_pos) - 4;
            u32 jw_mm = ((u32)((u32)jo_mm & 0x3FFFFu) << 6) | 0x06u;
            base[entry_off + jmm_pos    ] = (u8)jw_mm;
            base[entry_off + jmm_pos + 1] = (u8)(jw_mm >> 8);
            base[entry_off + jmm_pos + 2] = (u8)(jw_mm >> 16);

            inline_ops++; done = true;
        } else if (top == 0x2 && ((w >> 6) & 7) == 5 && mode == 3) {
            /* MOVE.L (An)+,(d16,Am) — bench-hot 0x2F5F (MOVE.L (SP)+,(d16,SP))
             * at 1000 hits/20 M cyc on the M6.77 corrected path (M6.80).
             *
             * Same shape as M6.76's MOVE.L (An)+,(Am)+ but with a
             * `(d16,Am)` destination instead of `(Am)+`. The 68k semantic
             * decodes src then dst: src ea_decode captures An_orig and
             * post-increments An; dst ea_decode captures the now-incremented
             * An (when src_an == dst_am) and adds d16.
             *
             *   Same An  (e.g. 0x2F5F):  write at  An_orig + 4 + d16
             *   Different An:            write at  dst_Am + d16
             *
             * Source admits RAM-or-ROM (M6.76 literals); destination is
             * RAM-only (the m68k_step helper covers ROM/MMIO writes, which
             * are typically no-ops anyway). */
            int dst_am = (w >> 9) & 7;
            int src_an = w & 7;
            u16 ext = mac_read16(cpu->mem, op_pc[i] + 2);
            i32 d16 = (i16)ext;
            bool same_an = (src_an == dst_am);

            emit_advance_flush(&e);
            emit_read_g(&e, &rc, G_A(src_an), 8);    /* a8 = src An (= An_orig) */

            /* Compute dst-addr base in a15.
             *   same_an    → a15 = a8 + 4  (the post-incr An_new)
             *   different  → a15 = dst_Am  (unchanged) */
            if (same_an) {
                xt_addi(&e, 15, 8, 4);
            } else {
                emit_read_g(&e, &rc, G_A(dst_am), 15);
            }
            /* Add d16 to a15 → final dst addr. */
            if (d16 >= -128 && d16 <= 127) {
                xt_addi(&e, 15, 15, d16);
            } else {
                emit_load_imm(&e, 11, 12, (u32)d16);
                xt_add (&e, 15, 15, 11);
            }

            /* Source bounds (RAM-or-ROM): a12 == 0 iff src ok. */
            emit_l32r_at(&e, 9, lit_off[LITERAL_RAM_BOUNDS],
                         entry_off + e.len);
            xt_and  (&e, 10, 8, 9);
            emit_l32r_at(&e, 9, lit_off[LITERAL_ROM_BOUNDS],
                         entry_off + e.len);
            xt_and  (&e, 11, 8, 9);
            emit_l32r_at(&e, 9, lit_off[LITERAL_ROM_BASE],
                         entry_off + e.len);
            xt_xor  (&e, 11, 11, 9);
            xt_and  (&e, 12, 10, 11);

            /* Dest bounds (RAM-only) folded into a12. */
            emit_l32r_at(&e, 9, lit_off[LITERAL_RAM_BOUNDS],
                         entry_off + e.len);
            xt_and  (&e, 9, 15, 9);
            xt_or   (&e, 12, 12, 9);
            emit_cache_flush(&e, &rc);
            i32 op_pc_mAd = 4, op_cyc_mAd = 8;
            xt_beqz (&e, 12, (i32)(6u + helper_step_after_flush_undo_size(&rc, op_pc_mAd, op_cyc_mAd)));
            emit_helper_step_after_flush_undo(&e, lit_off[HELPER_M68K_STEP],
                                              entry_off, &rc, op_pc_mAd, op_cyc_mAd);
            u32 jmAd_pos = e.len;
            xt_j    (&e, 4);

            /* === Fast path === */
            /* Pick src host base via a10 (M6.76 trick). */
            emit_l32r_at(&e, 9, lit_off[ADDR_RAM_BASE],
                         entry_off + e.len);
            xt_beqz (&e, 10, 6);
            emit_l32r_at(&e, 9, lit_off[ADDR_ROM_HOST_BASE],
                         entry_off + e.len);
            xt_add  (&e, 9, 9, 8);
            /* Read 4 BE bytes → a10 (.L). */
            xt_l8ui (&e, 11, 9, 0);
            xt_l8ui (&e, 12, 9, 1);
            xt_slli (&e, 10, 11, 24);
            xt_slli (&e, 12, 12, 16);
            xt_or   (&e, 10, 10, 12);
            xt_l8ui (&e, 11, 9, 2);
            xt_l8ui (&e, 12, 9, 3);
            xt_slli (&e, 11, 11, 8);
            xt_or   (&e, 10, 10, 11);
            xt_or   (&e, 10, 10, 12);

            /* Post-increment src An (commit). */
            xt_addi (&e, 8, 8, 4);
            emit_write_g(&e, &rc, G_A(src_an), 8);

            /* Dest host ptr: always RAM (dst bounds enforced above). */
            emit_l32r_at(&e, 9, lit_off[ADDR_RAM_BASE],
                         entry_off + e.len);
            xt_add  (&e, 9, 9, 15);

            /* Write 4 BE bytes from a10. */
            xt_extui(&e, 11, 10, 24, 7); xt_s8i(&e, 11, 9, 0);
            xt_extui(&e, 11, 10, 16, 7); xt_s8i(&e, 11, 9, 1);
            xt_extui(&e, 11, 10,  8, 7); xt_s8i(&e, 11, 9, 2);
            xt_extui(&e, 11, 10,  0, 7); xt_s8i(&e, 11, 9, 3);

            /* MOVE-family flags from the .L value. */
            if (!flags_dead[i]) emit_logic_flags(&e, 10);

            emit_advance(&e, op_pc_mAd, op_cyc_mAd);

            u32 mAd_here = e.len;
            i32 jo_mAd = (i32)(mAd_here - jmAd_pos) - 4;
            u32 jw_mAd = ((u32)((u32)jo_mAd & 0x3FFFFu) << 6) | 0x06u;
            base[entry_off + jmAd_pos    ] = (u8)jw_mAd;
            base[entry_off + jmAd_pos + 1] = (u8)(jw_mAd >> 8);
            base[entry_off + jmAd_pos + 2] = (u8)(jw_mAd >> 16);

            inline_ops++; done = true;
        } else if (top == 0x2 && ((w >> 6) & 7) == 0 && mode == 5) {
            /* MOVE.L (d16,An),Dn — bench-warm 0x202F+ at ~1000 helpers in
             * 20M cyc (M6.73). Stack-frame .L read pattern. EA = An +
             * sext16(d16); read 4 BE bytes into Dn; MOVE-family flags. */
            int dn = (w >> 9) & 7;
            int an = w & 7;
            u16 ext = mac_read16(cpu->mem, op_pc[i] + 2);
            i32 d16 = (i16)ext;

            emit_advance_flush(&e);
            emit_read_g(&e, &rc, G_A(an), 8);                /* a8 = An */
            if (d16 >= -128 && d16 <= 127) {
                xt_addi(&e, 8, 8, d16);
            } else {
                emit_load_imm(&e, 11, 12, (u32)d16);
                xt_add(&e, 8, 8, 11);
            }
            emit_l32r_at(&e, 9, lit_off[LITERAL_RAM_BOUNDS],
                         entry_off + e.len);
            xt_and  (&e, 10, 8, 9);
            emit_cache_flush(&e, &rc);
            i32 op_pc_dr = 4, op_cyc_dr = 8;
            xt_beqz (&e, 10, (i32)(6u + helper_step_after_flush_undo_size(&rc, op_pc_dr, op_cyc_dr)));
            emit_helper_step_after_flush_undo(&e, lit_off[HELPER_M68K_STEP],
                                              entry_off, &rc, op_pc_dr, op_cyc_dr);
            u32 jdr_pos = e.len;
            xt_j    (&e, 4);
            /* Fast path: 4 BE byte loads. */
            emit_l32r_at(&e, 9, lit_off[ADDR_RAM_BASE],
                         entry_off + e.len);
            xt_add  (&e, 9, 9, 8);                            /* a9 = ram + addr */
            xt_l8ui (&e, 11, 9, 0);
            xt_l8ui (&e, 12, 9, 1);
            xt_slli (&e, 10, 11, 24);
            xt_slli (&e, 12, 12, 16);
            xt_or   (&e, 10, 10, 12);
            xt_l8ui (&e, 11, 9, 2);
            xt_l8ui (&e, 12, 9, 3);
            xt_slli (&e, 11, 11, 8);
            xt_or   (&e, 10, 10, 11);
            xt_or   (&e, 10, 10, 12);                         /* a10 = .L */
            emit_write_g(&e, &rc, G_D(dn), 10);
            if (!flags_dead[i]) emit_logic_flags(&e, 10);
            emit_advance(&e, op_pc_dr, op_cyc_dr);

            u32 here_dr = e.len;
            i32 jo_dr = (i32)(here_dr - jdr_pos) - 4;
            u32 jw_dr = ((u32)((u32)jo_dr & 0x3FFFFu) << 6) | 0x06u;
            base[entry_off + jdr_pos    ] = (u8)jw_dr;
            base[entry_off + jdr_pos + 1] = (u8)(jw_dr >> 8);
            base[entry_off + jdr_pos + 2] = (u8)(jw_dr >> 16);

            inline_ops++; done = true;
        } else if (top == 0x1 && ((w >> 6) & 7) == 0 && mode == 5) {
            /* M6.91 — MOVE.B (d16,An),Dn — boot-hot 0x10A8 (12 K helpers /
             * 100 M cyc, top entry in the boot helper-histo).
             *
             * Bit-field decode (per memory/triple-differential.md):
             *   0x10A8 = 0001_0000_1010_1000
             *            top=1   dst_reg=0
             *                    dst_mode=000 (Dn)
             *                       src_mode=101 (d16,An)
             *                          src_reg=000 (A0)
             *          → MOVE.B (d16,A0),D0
             *
             * EA = An + sext16(d16). Read 1 byte from RAM or ROM; replace
             * Dn[7:0] preserving Dn[31:8]. MOVE-family flags from byte sign.
             *
             * Source admits RAM OR ROM via the M6.76 unified bounds shape
             * but with the BYTE variants (no `| 1` since MOVE.B has no
             * alignment requirement). The first attempt admitted only RAM —
             * it eliminated 268 boot helpers, leaving 11 866 ROM-source
             * hits still in the helper. Extending to ROM picks up the
             * bulk: boot's PC=0x400310 dispatches `MOVE.B (d16,A0),D0`
             * where A0 walks ROM-resident system tables. Cycles: m68k_step
             * base 4 + handler 4 = 8 cyc; length 4 (opcode + ext word). */
            int dn = (w >> 9) & 7;
            int an = w & 7;
            u16 ext = mac_read16(cpu->mem, op_pc[i] + 2);
            i32 d16 = (i16)ext;

            emit_advance_flush(&e);
            emit_read_g(&e, &rc, G_A(an), 8);
            if (d16 >= -128 && d16 <= 127) {
                xt_addi(&e, 8, 8, d16);
            } else {
                emit_load_imm(&e, 11, 12, (u32)d16);
                xt_add  (&e, 8, 8, 11);
            }

            /* Unified RAM-or-ROM byte bounds. a12 == 0 iff src in RAM or ROM. */
            emit_l32r_at(&e, 9, lit_off[LITERAL_RAM_BOUNDS_BYTE],
                         entry_off + e.len);
            xt_and  (&e, 10, 8, 9);                  /* a10 = src & RAM_MASK_B */
            emit_l32r_at(&e, 9, lit_off[LITERAL_ROM_BOUNDS_BYTE],
                         entry_off + e.len);
            xt_and  (&e, 11, 8, 9);
            emit_l32r_at(&e, 9, lit_off[LITERAL_ROM_BASE],
                         entry_off + e.len);
            xt_xor  (&e, 11, 11, 9);                 /* a11 = (src & ROM_M) ^ ROM_BASE */
            xt_and  (&e, 12, 10, 11);                /* a12 == 0 iff RAM-or-ROM */
            emit_cache_flush(&e, &rc);
            i32 op_pc_mb = 4, op_cyc_mb = 8;
            xt_beqz (&e, 12, (i32)(6u + helper_step_after_flush_undo_size(&rc, op_pc_mb, op_cyc_mb)));
            emit_helper_step_after_flush_undo(&e, lit_off[HELPER_M68K_STEP],
                                              entry_off, &rc, op_pc_mb, op_cyc_mb);
            u32 jmb_pos = e.len;
            xt_j    (&e, 4);

            /* Fast path: pick base via a10 (RAM_BASE when a10==0, else ROM). */
            emit_l32r_at(&e, 9, lit_off[ADDR_RAM_BASE],
                         entry_off + e.len);
            xt_beqz (&e, 10, 6);                     /* rel=6 skips one l32r */
            emit_l32r_at(&e, 9, lit_off[ADDR_ROM_HOST_BASE],
                         entry_off + e.len);
            xt_add  (&e, 9, 9, 8);
            xt_l8ui (&e, 10, 9, 0);                  /* a10 = byte (zero-ext) */
            emit_read_g(&e, &rc, G_D(dn), 11);       /* a11 = old Dn */
            xt_srli (&e, 11, 11, 8);
            xt_slli (&e, 11, 11, 8);                 /* a11 = Dn[31:8] | 0 in low 8 */
            xt_or   (&e, 11, 11, 10);                /* a11 = new Dn */
            emit_write_g(&e, &rc, G_D(dn), 11);
            if (!flags_dead[i]) {
                /* Shift byte to bit 31 so emit_logic_flags sees the .B sign
                 * bit at bit 31 (matches the .B / .W / .L convention). Z is
                 * preserved since shifting zero by 24 stays zero. */
                xt_slli (&e, 8, 10, 24);
                emit_logic_flags(&e, 8);
            }
            emit_advance(&e, op_pc_mb, op_cyc_mb);

            u32 here_mb = e.len;
            i32 jo_mb = (i32)(here_mb - jmb_pos) - 4;
            u32 jw_mb = ((u32)((u32)jo_mb & 0x3FFFFu) << 6) | 0x06u;
            base[entry_off + jmb_pos    ] = (u8)jw_mb;
            base[entry_off + jmb_pos + 1] = (u8)(jw_mb >> 8);
            base[entry_off + jmb_pos + 2] = (u8)(jw_mb >> 16);

            inline_ops++; done = true;
        } else if (top == 0x1 && ((w >> 6) & 7) == 2 && mode == 5) {
            /* M6.91 — MOVE.B (d16,An),(Am) mem-to-mem — the actual boot-hot
             * 0x10A8 (12 K helpers / 100 M cyc, top entry in boot's helper
             * histogram).
             *
             * Bit-field decode (per memory/cmp-vs-cmpa-decode.md):
             *   0x10A8 = 0001_0000_1010_1000
             *            top=1   dst_reg=0
             *                    dst_mode=010 (An)
             *                       src_mode=101 (d16,An)
             *                          src_reg=000 (A0)
             *          → MOVE.B (d16,A0),(A0)
             *
             * Src EA = An + sext16(d16) (RAM or ROM, byte access).
             * Dst EA = Am             (RAM only, byte access).
             * Read 1 byte → write 1 byte. MOVE-family flags from byte.
             *
             * Pairs with the M6.91 MOVE.B (d16,An),Dn arm just above;
             * shares the same byte-aligned bounds literals. */
            int dst_am = (w >> 9) & 7;
            int src_an = w & 7;
            u16 ext = mac_read16(cpu->mem, op_pc[i] + 2);
            i32 d16 = (i16)ext;

            emit_advance_flush(&e);
            emit_read_g(&e, &rc, G_A(src_an), 8);
            if (d16 >= -128 && d16 <= 127) {
                xt_addi(&e, 8, 8, d16);
            } else {
                emit_load_imm(&e, 11, 12, (u32)d16);
                xt_add  (&e, 8, 8, 11);
            }
            emit_read_g(&e, &rc, G_A(dst_am), 15);   /* a15 = dst An */

            /* Src bounds: RAM or ROM byte. a12 == 0 iff src RAM-or-ROM. */
            emit_l32r_at(&e, 9, lit_off[LITERAL_RAM_BOUNDS_BYTE],
                         entry_off + e.len);
            xt_and  (&e, 10, 8, 9);                  /* a10 = src & RAM_M_B */
            emit_l32r_at(&e, 9, lit_off[LITERAL_ROM_BOUNDS_BYTE],
                         entry_off + e.len);
            xt_and  (&e, 11, 8, 9);
            emit_l32r_at(&e, 9, lit_off[LITERAL_ROM_BASE],
                         entry_off + e.len);
            xt_xor  (&e, 11, 11, 9);                 /* a11 = (src & ROM_M) ^ ROM_BASE */
            xt_and  (&e, 12, 10, 11);                /* a12 = 0 iff RAM-or-ROM */

            /* Dst bounds: RAM byte only (writes can't go to ROM). */
            emit_l32r_at(&e, 9, lit_off[LITERAL_RAM_BOUNDS_BYTE],
                         entry_off + e.len);
            xt_and  (&e, 9, 15, 9);                  /* a9 = dst & RAM_M_B */
            xt_or   (&e, 12, 12, 9);                 /* a12 == 0 iff both pass */

            emit_cache_flush(&e, &rc);
            i32 op_pc_mb2 = 4, op_cyc_mb2 = 8;
            xt_beqz (&e, 12, (i32)(6u + helper_step_after_flush_undo_size(&rc, op_pc_mb2, op_cyc_mb2)));
            emit_helper_step_after_flush_undo(&e, lit_off[HELPER_M68K_STEP],
                                              entry_off, &rc, op_pc_mb2, op_cyc_mb2);
            u32 jmb2_pos = e.len;
            xt_j    (&e, 4);

            /* Fast path: src host ptr via a10 (RAM_BASE if a10==0, else ROM). */
            emit_l32r_at(&e, 9, lit_off[ADDR_RAM_BASE],
                         entry_off + e.len);
            xt_beqz (&e, 10, 6);                     /* rel=6 skips one l32r */
            emit_l32r_at(&e, 9, lit_off[ADDR_ROM_HOST_BASE],
                         entry_off + e.len);
            xt_add  (&e, 9, 9, 8);                   /* a9 = src host ptr */
            xt_l8ui (&e, 10, 9, 0);                  /* a10 = byte */

            /* Dst host ptr: always RAM. */
            emit_l32r_at(&e, 9, lit_off[ADDR_RAM_BASE],
                         entry_off + e.len);
            xt_add  (&e, 9, 9, 15);                  /* a9 = dst host ptr */
            xt_s8i  (&e, 10, 9, 0);                  /* write byte */

            if (!flags_dead[i]) {
                xt_slli (&e, 8, 10, 24);             /* sign of .B in bit 31 */
                emit_logic_flags(&e, 8);
            }
            emit_advance(&e, op_pc_mb2, op_cyc_mb2);

            u32 here_mb2 = e.len;
            i32 jo_mb2 = (i32)(here_mb2 - jmb2_pos) - 4;
            u32 jw_mb2 = ((u32)((u32)jo_mb2 & 0x3FFFFu) << 6) | 0x06u;
            base[entry_off + jmb2_pos    ] = (u8)jw_mb2;
            base[entry_off + jmb2_pos + 1] = (u8)(jw_mb2 >> 8);
            base[entry_off + jmb2_pos + 2] = (u8)(jw_mb2 >> 16);

            inline_ops++; done = true;
        } else if (top == 0x1 && ((w >> 6) & 7) == 0 && mode == 2) {
            /* M6.92 — MOVE.B (An),Dn — boot-warm 0x1211 (MOVE.B (A1),D1)
             * at 6 K helpers / 100 M cyc, plus 0x1411 (MOVE.B (A1),D2)
             * at 3 K. Same shape as the M6.91 (d16,An),Dn arm but with
             * no displacement to add. Src admits RAM or ROM byte; flags
             * from .B sign bit. Length 2, cycles 8. */
            int dn = (w >> 9) & 7;
            int an = w & 7;

            emit_advance_flush(&e);
            u8 an_reg = emit_read_g_in(&e, &rc, G_A(an), 8);

            /* Unified RAM-or-ROM byte bounds. a12 == 0 iff src RAM-or-ROM. */
            emit_l32r_at(&e, 9, lit_off[LITERAL_RAM_BOUNDS_BYTE],
                         entry_off + e.len);
            xt_and  (&e, 10, an_reg, 9);
            emit_l32r_at(&e, 9, lit_off[LITERAL_ROM_BOUNDS_BYTE],
                         entry_off + e.len);
            xt_and  (&e, 11, an_reg, 9);
            emit_l32r_at(&e, 9, lit_off[LITERAL_ROM_BASE],
                         entry_off + e.len);
            xt_xor  (&e, 11, 11, 9);
            xt_and  (&e, 12, 10, 11);
            emit_cache_flush(&e, &rc);
            i32 op_pc_b1 = 2, op_cyc_b1 = 8;
            xt_beqz (&e, 12, (i32)(6u + helper_step_after_flush_undo_size(&rc, op_pc_b1, op_cyc_b1)));
            emit_helper_step_after_flush_undo(&e, lit_off[HELPER_M68K_STEP],
                                              entry_off, &rc, op_pc_b1, op_cyc_b1);
            u32 jb1_pos = e.len;
            xt_j    (&e, 4);

            /* Fast path: src host ptr via a10, load byte, merge into Dn[7:0]. */
            emit_l32r_at(&e, 9, lit_off[ADDR_RAM_BASE],
                         entry_off + e.len);
            xt_beqz (&e, 10, 6);
            emit_l32r_at(&e, 9, lit_off[ADDR_ROM_HOST_BASE],
                         entry_off + e.len);
            xt_add  (&e, 9, 9, an_reg);
            xt_l8ui (&e, 10, 9, 0);
            emit_read_g(&e, &rc, G_D(dn), 11);
            xt_srli (&e, 11, 11, 8);
            xt_slli (&e, 11, 11, 8);
            xt_or   (&e, 11, 11, 10);
            emit_write_g(&e, &rc, G_D(dn), 11);
            if (!flags_dead[i]) {
                xt_slli (&e, 8, 10, 24);
                emit_logic_flags(&e, 8);
            }
            emit_advance(&e, op_pc_b1, op_cyc_b1);

            u32 here_b1 = e.len;
            i32 jo_b1 = (i32)(here_b1 - jb1_pos) - 4;
            u32 jw_b1 = ((u32)((u32)jo_b1 & 0x3FFFFu) << 6) | 0x06u;
            base[entry_off + jb1_pos    ] = (u8)jw_b1;
            base[entry_off + jb1_pos + 1] = (u8)(jw_b1 >> 8);
            base[entry_off + jb1_pos + 2] = (u8)(jw_b1 >> 16);

            inline_ops++; done = true;
        } else if (top == 0x1 && ((w >> 6) & 7) == 0 && mode == 3) {
            /* M6.94 — MOVE.B (An)+,Dn — boot-warm 0x1218 (MOVE.B (A0)+,D1)
             * at 1.1 K helpers / 100 M cyc, plus variants. Same shape as
             * M6.92's MOVE.B (An),Dn but with An post-increment by 1.
             * The post-incr commits AFTER the byte read so a same-An
             * edge case (e.g. MOVE.B (A0)+,D0) reads the byte at original
             * A0 first, then increments. Length 2, cycles 8. */
            int dn = (w >> 9) & 7;
            int an = w & 7;

            emit_advance_flush(&e);
            emit_read_g(&e, &rc, G_A(an), 8);   /* a8 = An (pre-incr) */

            /* Unified RAM-or-ROM byte bounds. */
            emit_l32r_at(&e, 9, lit_off[LITERAL_RAM_BOUNDS_BYTE],
                         entry_off + e.len);
            xt_and  (&e, 10, 8, 9);
            emit_l32r_at(&e, 9, lit_off[LITERAL_ROM_BOUNDS_BYTE],
                         entry_off + e.len);
            xt_and  (&e, 11, 8, 9);
            emit_l32r_at(&e, 9, lit_off[LITERAL_ROM_BASE],
                         entry_off + e.len);
            xt_xor  (&e, 11, 11, 9);
            xt_and  (&e, 12, 10, 11);
            emit_cache_flush(&e, &rc);
            i32 op_pc_b3 = 2, op_cyc_b3 = 8;
            xt_beqz (&e, 12, (i32)(6u + helper_step_after_flush_undo_size(&rc, op_pc_b3, op_cyc_b3)));
            emit_helper_step_after_flush_undo(&e, lit_off[HELPER_M68K_STEP],
                                              entry_off, &rc, op_pc_b3, op_cyc_b3);
            u32 jb3_pos = e.len;
            xt_j    (&e, 4);

            /* Fast path: read byte, merge into Dn[7:0], commit An+1. */
            emit_l32r_at(&e, 9, lit_off[ADDR_RAM_BASE],
                         entry_off + e.len);
            xt_beqz (&e, 10, 6);
            emit_l32r_at(&e, 9, lit_off[ADDR_ROM_HOST_BASE],
                         entry_off + e.len);
            xt_add  (&e, 9, 9, 8);
            xt_l8ui (&e, 10, 9, 0);
            /* Post-incr An — commit AFTER read. */
            xt_addi (&e, 8, 8, 1);
            emit_write_g(&e, &rc, G_A(an), 8);
            /* Merge byte into Dn[7:0]. */
            emit_read_g(&e, &rc, G_D(dn), 11);
            xt_srli (&e, 11, 11, 8);
            xt_slli (&e, 11, 11, 8);
            xt_or   (&e, 11, 11, 10);
            emit_write_g(&e, &rc, G_D(dn), 11);
            if (!flags_dead[i]) {
                xt_slli (&e, 8, 10, 24);
                emit_logic_flags(&e, 8);
            }
            emit_advance(&e, op_pc_b3, op_cyc_b3);

            u32 here_b3 = e.len;
            i32 jo_b3 = (i32)(here_b3 - jb3_pos) - 4;
            u32 jw_b3 = ((u32)((u32)jo_b3 & 0x3FFFFu) << 6) | 0x06u;
            base[entry_off + jb3_pos    ] = (u8)jw_b3;
            base[entry_off + jb3_pos + 1] = (u8)(jw_b3 >> 8);
            base[entry_off + jb3_pos + 2] = (u8)(jw_b3 >> 16);

            inline_ops++; done = true;
        } else if (top == 0x1 && ((w >> 6) & 7) == 3 && mode == 0) {
            /* M6.94 — MOVE.B Dn,(An)+ — sibling of MOVE.B Dn,(An) with
             * An post-increment by 1. Length 2, cycles 8. */
            int dst_an = (w >> 9) & 7;
            int dn = w & 7;

            emit_advance_flush(&e);
            emit_read_g(&e, &rc, G_A(dst_an), 8);   /* a8 = An (pre-incr) */

            /* Dst bounds: RAM byte only. */
            emit_l32r_at(&e, 9, lit_off[LITERAL_RAM_BOUNDS_BYTE],
                         entry_off + e.len);
            xt_and  (&e, 10, 8, 9);
            emit_cache_flush(&e, &rc);
            i32 op_pc_b4 = 2, op_cyc_b4 = 8;
            xt_beqz (&e, 10, (i32)(6u + helper_step_after_flush_undo_size(&rc, op_pc_b4, op_cyc_b4)));
            emit_helper_step_after_flush_undo(&e, lit_off[HELPER_M68K_STEP],
                                              entry_off, &rc, op_pc_b4, op_cyc_b4);
            u32 jb4_pos = e.len;
            xt_j    (&e, 4);

            /* Fast path: read Dn byte, store, post-incr An. */
            emit_l32r_at(&e, 9, lit_off[ADDR_RAM_BASE],
                         entry_off + e.len);
            xt_add  (&e, 9, 9, 8);
            emit_read_g(&e, &rc, G_D(dn), 10);
            xt_extui(&e, 11, 10, 0, 7);
            xt_s8i  (&e, 11, 9, 0);
            /* Post-incr An. */
            xt_addi (&e, 8, 8, 1);
            emit_write_g(&e, &rc, G_A(dst_an), 8);
            if (!flags_dead[i]) {
                xt_slli (&e, 8, 11, 24);
                emit_logic_flags(&e, 8);
            }
            emit_advance(&e, op_pc_b4, op_cyc_b4);

            u32 here_b4 = e.len;
            i32 jo_b4 = (i32)(here_b4 - jb4_pos) - 4;
            u32 jw_b4 = ((u32)((u32)jo_b4 & 0x3FFFFu) << 6) | 0x06u;
            base[entry_off + jb4_pos    ] = (u8)jw_b4;
            base[entry_off + jb4_pos + 1] = (u8)(jw_b4 >> 8);
            base[entry_off + jb4_pos + 2] = (u8)(jw_b4 >> 16);

            inline_ops++; done = true;
        } else if (top == 0x1 && ((w >> 6) & 7) == 2 && mode == 0) {
            /* M6.92 — MOVE.B Dn,(An) — boot-warm 0x1082 (MOVE.B D2,(A0))
             * at 3 K helpers / 100 M cyc. Sibling of MOVE.B (An),Dn but
             * with the directions swapped — src is Dn (no bounds check),
             * dst is (An) (RAM byte bounds). Flags from byte value. */
            int dst_an = (w >> 9) & 7;
            int dn = w & 7;

            emit_advance_flush(&e);
            u8 an_reg = emit_read_g_in(&e, &rc, G_A(dst_an), 8);

            /* Dst bounds: RAM byte only (writes can't go to ROM). */
            emit_l32r_at(&e, 9, lit_off[LITERAL_RAM_BOUNDS_BYTE],
                         entry_off + e.len);
            xt_and  (&e, 10, an_reg, 9);
            emit_cache_flush(&e, &rc);
            i32 op_pc_b2 = 2, op_cyc_b2 = 8;
            xt_beqz (&e, 10, (i32)(6u + helper_step_after_flush_undo_size(&rc, op_pc_b2, op_cyc_b2)));
            emit_helper_step_after_flush_undo(&e, lit_off[HELPER_M68K_STEP],
                                              entry_off, &rc, op_pc_b2, op_cyc_b2);
            u32 jb2_pos = e.len;
            xt_j    (&e, 4);

            /* Fast path: dst host ptr, read Dn byte, store. */
            emit_l32r_at(&e, 9, lit_off[ADDR_RAM_BASE],
                         entry_off + e.len);
            xt_add  (&e, 9, 9, an_reg);
            emit_read_g(&e, &rc, G_D(dn), 10);       /* a10 = Dn */
            xt_extui(&e, 11, 10, 0, 7);              /* a11 = Dn[7:0] */
            xt_s8i  (&e, 11, 9, 0);
            if (!flags_dead[i]) {
                xt_slli (&e, 8, 11, 24);             /* byte at bit 31 */
                emit_logic_flags(&e, 8);
            }
            emit_advance(&e, op_pc_b2, op_cyc_b2);

            u32 here_b2 = e.len;
            i32 jo_b2 = (i32)(here_b2 - jb2_pos) - 4;
            u32 jw_b2 = ((u32)((u32)jo_b2 & 0x3FFFFu) << 6) | 0x06u;
            base[entry_off + jb2_pos    ] = (u8)jw_b2;
            base[entry_off + jb2_pos + 1] = (u8)(jw_b2 >> 8);
            base[entry_off + jb2_pos + 2] = (u8)(jw_b2 >> 16);

            inline_ops++; done = true;
        } else if (top == 0x2 && ((w >> 6) & 7) == 1 && mode == 5) {
            /* MOVEA.L (d16,An),Am — bench-warm 0x246F (MOVEA.L (d16,SP),A3)
             * at ~13 K helpers in 20M cyc (M6.75). Sibling of the M6.73
             * MOVE.L (d16,An),Dn arm — same address compute + .L read,
             * but writes 32-bit result to Am and skips the flag emit
             * (MOVEA never touches CCR). */
            int dst_am = (w >> 9) & 7;
            int src_an = w & 7;
            u16 ext = mac_read16(cpu->mem, op_pc[i] + 2);
            i32 d16 = (i16)ext;

            emit_advance_flush(&e);
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
            emit_cache_flush(&e, &rc);
            i32 op_pc_mAd = 4, op_cyc_mAd = 8;
            xt_beqz (&e, 10, (i32)(6u + helper_step_after_flush_undo_size(&rc, op_pc_mAd, op_cyc_mAd)));
            emit_helper_step_after_flush_undo(&e, lit_off[HELPER_M68K_STEP],
                                              entry_off, &rc, op_pc_mAd, op_cyc_mAd);
            u32 jmAd_pos = e.len;
            xt_j    (&e, 4);
            /* Fast path: 4 BE byte loads. */
            emit_l32r_at(&e, 9, lit_off[ADDR_RAM_BASE],
                         entry_off + e.len);
            xt_add  (&e, 9, 9, 8);
            xt_l8ui (&e, 11, 9, 0);
            xt_l8ui (&e, 12, 9, 1);
            xt_slli (&e, 10, 11, 24);
            xt_slli (&e, 12, 12, 16);
            xt_or   (&e, 10, 10, 12);
            xt_l8ui (&e, 11, 9, 2);
            xt_l8ui (&e, 12, 9, 3);
            xt_slli (&e, 11, 11, 8);
            xt_or   (&e, 10, 10, 11);
            xt_or   (&e, 10, 10, 12);                         /* a10 = .L */
            emit_write_g(&e, &rc, G_A(dst_am), 10);
            /* MOVEA — no flags. */
            emit_advance(&e, op_pc_mAd, op_cyc_mAd);

            u32 here_mAd = e.len;
            i32 jo_mAd = (i32)(here_mAd - jmAd_pos) - 4;
            u32 jw_mAd = ((u32)((u32)jo_mAd & 0x3FFFFu) << 6) | 0x06u;
            base[entry_off + jmAd_pos    ] = (u8)jw_mAd;
            base[entry_off + jmAd_pos + 1] = (u8)(jw_mAd >> 8);
            base[entry_off + jmAd_pos + 2] = (u8)(jw_mAd >> 16);

            inline_ops++; done = true;
        } else if (top == 0xD && szf == 3 && ((w >> 8) & 1) && mode == 5) {
            /* ADDA.L (d16,An),Am — bench-hot 0xD1EE (ADDA.L (d16,A6),A0)
             * at 2000 hits/20 M cyc on the M6.77 corrected path (M6.79).
             * Mis-labelled as "ADD.L Dn,<ea>" in earlier STATUS notes;
             * the bit-field decode confirms it's actually ADDA.L (opmode
             * 111 = bits 8-6 = 111). Sibling of M6.75 MOVEA.L (d16,An),Am
             * — same address compute and .L read, but adds the loaded
             * .L to the destination An instead of replacing it. ADDA
             * never touches CCR. Source can be in ROM (bench's pointer
             * tables at PC ≈ 0x408???), so use the M6.76 unified
             * RAM-or-ROM bounds check + base selector.
             *
             * Length 4 (opword + d16), 12 cycles (interp base 4 +
             * handler 8 — note the ADDA path adds 8, vs the plain
             * MOVE/MOVEA path's 4). */
            int dst_am = (w >> 9) & 7;
            int src_an = w & 7;
            u16 ext = mac_read16(cpu->mem, op_pc[i] + 2);
            i32 d16 = (i16)ext;

            emit_advance_flush(&e);
            emit_read_g(&e, &rc, G_A(src_an), 8);
            if (d16 >= -128 && d16 <= 127) {
                xt_addi(&e, 8, 8, d16);
            } else {
                emit_load_imm(&e, 11, 12, (u32)d16);
                xt_add (&e, 8, 8, 11);
            }
            /* Unified RAM-or-ROM bounds (M6.76 shape). */
            emit_l32r_at(&e, 9, lit_off[LITERAL_RAM_BOUNDS],
                         entry_off + e.len);
            xt_and  (&e, 10, 8, 9);
            emit_l32r_at(&e, 9, lit_off[LITERAL_ROM_BOUNDS],
                         entry_off + e.len);
            xt_and  (&e, 11, 8, 9);
            emit_l32r_at(&e, 9, lit_off[LITERAL_ROM_BASE],
                         entry_off + e.len);
            xt_xor  (&e, 11, 11, 9);
            xt_and  (&e, 12, 10, 11);
            emit_cache_flush(&e, &rc);
            i32 op_pc_aAd = 4, op_cyc_aAd = 12;
            xt_beqz (&e, 12, (i32)(6u + helper_step_after_flush_undo_size(&rc, op_pc_aAd, op_cyc_aAd)));
            emit_helper_step_after_flush_undo(&e, lit_off[HELPER_M68K_STEP],
                                              entry_off, &rc, op_pc_aAd, op_cyc_aAd);
            u32 jaAd_pos = e.len;
            xt_j    (&e, 4);
            /* Fast path. Pick base via a10: a10==0 → RAM, else ROM. */
            emit_l32r_at(&e, 9, lit_off[ADDR_RAM_BASE],
                         entry_off + e.len);
            xt_beqz (&e, 10, 6);
            emit_l32r_at(&e, 9, lit_off[ADDR_ROM_HOST_BASE],
                         entry_off + e.len);
            xt_add  (&e, 9, 9, 8);
            xt_l8ui (&e, 11, 9, 0);
            xt_l8ui (&e, 12, 9, 1);
            xt_slli (&e, 10, 11, 24);
            xt_slli (&e, 12, 12, 16);
            xt_or   (&e, 10, 10, 12);
            xt_l8ui (&e, 11, 9, 2);
            xt_l8ui (&e, 12, 9, 3);
            xt_slli (&e, 11, 11, 8);
            xt_or   (&e, 10, 10, 11);
            xt_or   (&e, 10, 10, 12);            /* a10 = source .L */
            /* Add to dst An (full 32-bit, no flags). */
            emit_read_g(&e, &rc, G_A(dst_am), 11);
            xt_add  (&e, 11, 11, 10);
            emit_write_g(&e, &rc, G_A(dst_am), 11);
            emit_advance(&e, op_pc_aAd, op_cyc_aAd);

            u32 here_aAd = e.len;
            i32 jo_aAd = (i32)(here_aAd - jaAd_pos) - 4;
            u32 jw_aAd = ((u32)((u32)jo_aAd & 0x3FFFFu) << 6) | 0x06u;
            base[entry_off + jaAd_pos    ] = (u8)jw_aAd;
            base[entry_off + jaAd_pos + 1] = (u8)(jw_aAd >> 8);
            base[entry_off + jaAd_pos + 2] = (u8)(jw_aAd >> 16);

            inline_ops++; done = true;
        } else if (top == 0x2 && ((w >> 6) & 7) == 5 && (mode == 0 || mode == 1)) {
            /* MOVE.L Dn|Am,(d16,An) — bench-warm 0x2F41+ at ~1000 helpers in
             * 20M cyc (M6.73). Stack-frame .L write. EA = An + sext16(d16);
             * write 4 BE bytes; MOVE-family flags. */
            int dst_an = (w >> 9) & 7;
            int dm = w & 7;
            int g_src = (mode == 1) ? G_A(dm) : G_D(dm);
            u16 ext = mac_read16(cpu->mem, op_pc[i] + 2);
            i32 d16 = (i16)ext;

            emit_advance_flush(&e);
            emit_read_g(&e, &rc, G_A(dst_an), 8);             /* a8 = An */
            if (d16 >= -128 && d16 <= 127) {
                xt_addi(&e, 8, 8, d16);
            } else {
                emit_load_imm(&e, 11, 12, (u32)d16);
                xt_add(&e, 8, 8, 11);
            }
            emit_l32r_at(&e, 9, lit_off[LITERAL_RAM_BOUNDS],
                         entry_off + e.len);
            xt_and  (&e, 10, 8, 9);
            emit_cache_flush(&e, &rc);
            i32 op_pc_dw = 4, op_cyc_dw = 8;
            xt_beqz (&e, 10, (i32)(6u + helper_step_after_flush_undo_size(&rc, op_pc_dw, op_cyc_dw)));
            emit_helper_step_after_flush_undo(&e, lit_off[HELPER_M68K_STEP],
                                              entry_off, &rc, op_pc_dw, op_cyc_dw);
            u32 jdw_pos = e.len;
            xt_j    (&e, 4);
            /* Fast path: 4 BE byte stores. */
            emit_read_g(&e, &rc, g_src, 10);                  /* a10 = src .L */
            emit_l32r_at(&e, 9, lit_off[ADDR_RAM_BASE],
                         entry_off + e.len);
            xt_add  (&e, 9, 9, 8);
            xt_extui(&e, 11, 10, 24, 7); xt_s8i(&e, 11, 9, 0);
            xt_extui(&e, 11, 10, 16, 7); xt_s8i(&e, 11, 9, 1);
            xt_extui(&e, 11, 10,  8, 7); xt_s8i(&e, 11, 9, 2);
            xt_extui(&e, 11, 10,  0, 7); xt_s8i(&e, 11, 9, 3);
            if (!flags_dead[i]) emit_logic_flags(&e, 10);
            emit_advance(&e, op_pc_dw, op_cyc_dw);

            u32 here_dw = e.len;
            i32 jo_dw = (i32)(here_dw - jdw_pos) - 4;
            u32 jw_dw = ((u32)((u32)jo_dw & 0x3FFFFu) << 6) | 0x06u;
            base[entry_off + jdw_pos    ] = (u8)jw_dw;
            base[entry_off + jdw_pos + 1] = (u8)(jw_dw >> 8);
            base[entry_off + jdw_pos + 2] = (u8)(jw_dw >> 16);

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
        } else if (top == 0x3 && ((w >> 6) & 7) == 4 && mode == 0) {
            /* MOVE.W Dn,-(An) — bench-warm 0x3B40+ at ~1500 helpers in 20M
             * cyc (M6.73). Pre-decrement An by 2, write Dn's low 16 bits BE
             * at the new address, MOVE-family flags. Mirrors MOVE.W Dn,(An)
             * with the pre-dec. */
            int an = (w >> 9) & 7;
            int dn = w & 7;

            emit_advance_flush(&e);                  /* before An read */
            emit_read_g(&e, &rc, G_A(an), 8);
            xt_addi(&e, 8, 8, -2);                   /* pre-decrement */
            emit_l32r_at(&e, 9, lit_off[LITERAL_RAM_BOUNDS],
                         entry_off + e.len);
            xt_and  (&e, 10, 8, 9);
            emit_cache_flush(&e, &rc);
            i32 op_pc_pdw = 2, op_cyc_pdw = 8;
            xt_beqz (&e, 10, (i32)(6u + helper_step_after_flush_undo_size(&rc, op_pc_pdw, op_cyc_pdw)));

            /* Helper. */
            emit_helper_step_after_flush_undo(&e, lit_off[HELPER_M68K_STEP],
                                              entry_off, &rc, op_pc_pdw, op_cyc_pdw);
            u32 jpdw_pos = e.len;
            xt_j    (&e, 4);

            /* Fast path: write .W BE bytes, commit pre-decremented An. */
            emit_read_g(&e, &rc, G_D(dn), 10);       /* a10 = Dn */
            xt_extui(&e, 11, 10, 8, 7);              /* a11 = .W high */
            xt_extui(&e, 12, 10, 0, 7);              /* a12 = .W low  */
            emit_l32r_at(&e, 9, lit_off[ADDR_RAM_BASE],
                         entry_off + e.len);
            xt_add  (&e, 9, 9, 8);
            xt_s8i  (&e, 11, 9, 0);
            xt_s8i  (&e, 12, 9, 1);
            emit_write_g(&e, &rc, G_A(an), 8);       /* commit -2 An */
            if (!flags_dead[i]) {
                xt_slli (&e, 8, 10, 16);
                emit_logic_flags(&e, 8);
            }
            emit_advance(&e, op_pc_pdw, op_cyc_pdw);

            u32 here_pdw = e.len;
            i32 jo_pdw = (i32)(here_pdw - jpdw_pos) - 4;
            u32 jw_pdw = ((u32)((u32)jo_pdw & 0x3FFFFu) << 6) | 0x06u;
            base[entry_off + jpdw_pos    ] = (u8)jw_pdw;
            base[entry_off + jpdw_pos + 1] = (u8)(jw_pdw >> 8);
            base[entry_off + jpdw_pos + 2] = (u8)(jw_pdw >> 16);

            inline_ops++; done = true;
        } else if (top == 0x4 && ((w >> 8) & 0xF) == 0x2 && szf == 2 && mode == 3) {
            /* CLR.L (An)+ — bench-warm 0x4299 (CLR.L (A1)+) at ~26 K helpers
             * in 20M cyc (M6.74). Long sibling of M6.73's CLR.W (An)+. Same
             * shape but writes 4 zero bytes, post-increments An by 4. CLR
             * cycle accounting is size-independent in the interp (base 4 +
             * handler 4 = 8 cyc). */
            int an = w & 7;

            emit_advance_flush(&e);
            emit_read_g(&e, &rc, G_A(an), 8);
            emit_l32r_at(&e, 9, lit_off[LITERAL_RAM_BOUNDS],
                         entry_off + e.len);
            xt_and  (&e, 10, 8, 9);
            emit_cache_flush(&e, &rc);
            i32 op_pc_clrl = 2, op_cyc_clrl = 8;
            xt_beqz (&e, 10, (i32)(6u + helper_step_after_flush_undo_size(&rc, op_pc_clrl, op_cyc_clrl)));

            emit_helper_step_after_flush_undo(&e, lit_off[HELPER_M68K_STEP],
                                              entry_off, &rc, op_pc_clrl, op_cyc_clrl);
            u32 jclrl_pos = e.len;
            xt_j    (&e, 4);

            /* Fast path: write 4 zero bytes at [ram_base + An], An += 4. */
            emit_l32r_at(&e, 9, lit_off[ADDR_RAM_BASE],
                         entry_off + e.len);
            xt_add  (&e, 9, 9, 8);
            xt_movi (&e, 11, 0);
            xt_s8i  (&e, 11, 9, 0);
            xt_s8i  (&e, 11, 9, 1);
            xt_s8i  (&e, 11, 9, 2);
            xt_s8i  (&e, 11, 9, 3);
            xt_addi (&e, 8, 8, 4);
            emit_write_g(&e, &rc, G_A(an), 8);
            if (!flags_dead[i]) {
                xt_movi (&e, 12, -16);
                xt_and  (&e, R_SR, R_SR, 12);
                xt_movi (&e, 12, 0x04);
                xt_or   (&e, R_SR, R_SR, 12);
                g_sr_dirty = true;
                sext_memo_invalidate();
            }
            emit_advance(&e, op_pc_clrl, op_cyc_clrl);

            u32 here_clrl = e.len;
            i32 jo_clrl = (i32)(here_clrl - jclrl_pos) - 4;
            u32 jw_clrl = ((u32)((u32)jo_clrl & 0x3FFFFu) << 6) | 0x06u;
            base[entry_off + jclrl_pos    ] = (u8)jw_clrl;
            base[entry_off + jclrl_pos + 1] = (u8)(jw_clrl >> 8);
            base[entry_off + jclrl_pos + 2] = (u8)(jw_clrl >> 16);

            inline_ops++; done = true;
        } else if (top == 0x4 && ((w >> 8) & 0xF) == 0x2 && szf == 1 && mode == 3) {
            /* CLR.W (An)+ — bench-hot 0x4258 at ~23 % of all helpers (M6.73).
             * Speedometer runs a memset-zero loop body that hammers this.
             * Same shape as MOVE.W Dn,(An) but the value is a hard-coded 0,
             * with a 2-byte post-increment on An, and a CLR-specific flag
             * emit (Z=1, N=V=C=0, X preserved). */
            int an = w & 7;

            emit_advance_flush(&e);                  /* before An read */
            emit_read_g(&e, &rc, G_A(an), 8);
            emit_l32r_at(&e, 9, lit_off[LITERAL_RAM_BOUNDS],
                         entry_off + e.len);
            xt_and  (&e, 10, 8, 9);
            emit_cache_flush(&e, &rc);   /* before conditional helper */
            i32 op_pc_clr = 2, op_cyc_clr = 8;
            xt_beqz (&e, 10, (i32)(6u + helper_step_after_flush_undo_size(&rc, op_pc_clr, op_cyc_clr)));

            /* Helper. */
            emit_helper_step_after_flush_undo(&e, lit_off[HELPER_M68K_STEP],
                                              entry_off, &rc, op_pc_clr, op_cyc_clr);
            u32 jclr_pos = e.len;
            xt_j    (&e, 4);

            /* Fast path: write two zero bytes at [ram_base + An], An += 2. */
            emit_l32r_at(&e, 9, lit_off[ADDR_RAM_BASE],
                         entry_off + e.len);
            xt_add  (&e, 9, 9, 8);
            xt_movi (&e, 11, 0);
            xt_s8i  (&e, 11, 9, 0);
            xt_s8i  (&e, 11, 9, 1);
            /* Post-increment An by 2. a8 still holds pre-incr An. */
            xt_addi (&e, 8, 8, 2);
            emit_write_g(&e, &rc, G_A(an), 8);
            if (!flags_dead[i]) {
                /* CLR flags: clear X-preserve mask (-16 keeps bit 4 and up),
                 * then OR in Z=0x04. Cheaper than emit_logic_flags(zero). */
                xt_movi (&e, 12, -16);
                xt_and  (&e, R_SR, R_SR, 12);
                xt_movi (&e, 12, 0x04);
                xt_or   (&e, R_SR, R_SR, 12);
                g_sr_dirty = true;
                sext_memo_invalidate();
            }
            emit_advance(&e, op_pc_clr, op_cyc_clr);

            u32 here_clr = e.len;
            i32 jo_clr = (i32)(here_clr - jclr_pos) - 4;
            u32 jw_clr = ((u32)((u32)jo_clr & 0x3FFFFu) << 6) | 0x06u;
            base[entry_off + jclr_pos    ] = (u8)jw_clr;
            base[entry_off + jclr_pos + 1] = (u8)(jw_clr >> 8);
            base[entry_off + jclr_pos + 2] = (u8)(jw_clr >> 16);

            inline_ops++; done = true;
        } else if (top == 0xB && szf == 3 && ((w >> 8) & 1) == 1 && (mode == 0 || mode == 1)) {
            /* CMPA.L (Dn|An),An — bench-hot 0xB1C9 (CMPA.L A1,A0) at
             * 21 % of 20M-cyc helpers (M6.74). 32-bit register-source
             * compare: An_dst - <src.L>. Sets full N/Z/V/C, leaves X
             * unchanged (CMPA never writes X). 2-byte instruction;
             * 10 cycles (interp base 4 + handler 6). */
            int dst_an = (w >> 9) & 7;
            int src_reg = w & 7;
            int g_src = (mode == 1) ? G_A(src_reg) : G_D(src_reg);
            emit_read_g(&e, &rc, g_src, 8);            /* a8 = src .L */
            emit_read_g(&e, &rc, G_A(dst_an), 9);      /* a9 = dst .L */
            xt_sub(&e, 10, 9, 8);                       /* a10 = d - s */
            emit_addsub_flags_long_masked(&e, true, true, 8, 9, 10, flags_needed[i]);
            emit_advance(&e, 2, 10);
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
        } else if (top == 0x3 && ((w >> 6) & 7) == 7 && ((w >> 9) & 7) == 0 && mode == 0) {
            /* M6.110 — MOVE.W Dn,(xxx).W — bench-hot 0x31C0 at 21 K
             * helpers / 100 M cyc on bench's post-cycle-11898 path.
             * Destination side of the M6.109 (xxx).W class.
             *
             * Compile-time RAM check on the abs address. On RAM hit,
             * emit 2-byte BE write directly. MOVE-family flags from
             * the .W value. Length 4, cycles 8. */
            int dn = w & 7;
            u16 ext = mac_read16(cpu->mem, op_pc[i] + 2);
            u32 abs_addr = (u32)(i32)(i16)ext;
            abs_addr &= 0xFFFFFFu;

            u32 ram_size = cpu->mem ? cpu->mem->ram_size : 0;
            bool overlay = cpu->mem ? cpu->mem->overlay : true;
            bool ram_pow2 = ram_size > 0 && (ram_size & (ram_size - 1)) == 0;
            bool addr_in_ram = !overlay && ram_pow2
                               && abs_addr < ram_size
                               && (abs_addr & 1) == 0;

            if (addr_in_ram) {
                emit_advance_flush(&e);
                emit_l32r_at(&e, 9, lit_off[ADDR_RAM_BASE],
                             entry_off + e.len);
                if ((i32)abs_addr >= -128 && (i32)abs_addr <= 127) {
                    xt_addi(&e, 9, 9, (i32)abs_addr);
                } else if ((i32)abs_addr >= -2048 && (i32)abs_addr <= 2047) {
                    xt_movi(&e, 10, (i32)abs_addr);
                    xt_add (&e, 9, 9, 10);
                } else {
                    emit_load_imm(&e, 10, 11, abs_addr);
                    xt_add (&e, 9, 9, 10);
                }
                /* Read Dn (cache slot direct if cached). */
                u8 dn_reg = emit_read_g_in(&e, &rc, G_D(dn), 10);
                xt_extui(&e, 11, dn_reg, 8, 7);             /* .W high byte */
                xt_extui(&e, 12, dn_reg, 0, 7);             /* .W low byte */
                xt_s8i  (&e, 11, 9, 0);
                xt_s8i  (&e, 12, 9, 1);
                if (!flags_dead[i]) {
                    xt_slli(&e, 8, dn_reg, 16);             /* .W's sign at bit 31 */
                    emit_logic_flags(&e, 8);
                }
                emit_advance(&e, 4, 8);
                inline_ops++; done = true;
            }
        } else if (top == 0x1 && ((w >> 6) & 7) == 7 && ((w >> 9) & 7) == 0 && mode == 0) {
            /* M6.110 — MOVE.B Dn,(xxx).W. Sibling of MOVE.W Dn,(xxx).W
             * for byte writes. No alignment requirement. */
            int dn = w & 7;
            u16 ext = mac_read16(cpu->mem, op_pc[i] + 2);
            u32 abs_addr = (u32)(i32)(i16)ext;
            abs_addr &= 0xFFFFFFu;

            u32 ram_size = cpu->mem ? cpu->mem->ram_size : 0;
            bool overlay = cpu->mem ? cpu->mem->overlay : true;
            bool ram_pow2 = ram_size > 0 && (ram_size & (ram_size - 1)) == 0;
            bool addr_in_ram = !overlay && ram_pow2 && abs_addr < ram_size;

            if (addr_in_ram) {
                emit_advance_flush(&e);
                emit_l32r_at(&e, 9, lit_off[ADDR_RAM_BASE],
                             entry_off + e.len);
                if ((i32)abs_addr >= -128 && (i32)abs_addr <= 127) {
                    xt_addi(&e, 9, 9, (i32)abs_addr);
                } else if ((i32)abs_addr >= -2048 && (i32)abs_addr <= 2047) {
                    xt_movi(&e, 10, (i32)abs_addr);
                    xt_add (&e, 9, 9, 10);
                } else {
                    emit_load_imm(&e, 10, 11, abs_addr);
                    xt_add (&e, 9, 9, 10);
                }
                u8 dn_reg = emit_read_g_in(&e, &rc, G_D(dn), 10);
                xt_extui(&e, 11, dn_reg, 0, 7);             /* byte */
                xt_s8i  (&e, 11, 9, 0);
                if (!flags_dead[i]) {
                    xt_slli(&e, 8, 11, 24);
                    emit_logic_flags(&e, 8);
                }
                emit_advance(&e, 4, 8);
                inline_ops++; done = true;
            }
        } else if (top == 0x3 && ((w >> 6) & 7) == 0 && mode == 7 && (w & 7) == 0) {
            /* M6.109 — MOVE.W (xxx).W,Dn — sibling of M6.108's MOVE.L
             * (xxx).W form, for .W reads from a static low-RAM address.
             * Same compile-time RAM check; on RAM hit, inline 2-byte
             * BE read + merge into Dn[15:0]. Length 4, cycles 8. */
            int dn = (w >> 9) & 7;
            u16 ext = mac_read16(cpu->mem, op_pc[i] + 2);
            u32 abs_addr = (u32)(i32)(i16)ext;
            abs_addr &= 0xFFFFFFu;

            u32 ram_size = cpu->mem ? cpu->mem->ram_size : 0;
            bool overlay = cpu->mem ? cpu->mem->overlay : true;
            bool ram_pow2 = ram_size > 0 && (ram_size & (ram_size - 1)) == 0;
            bool addr_in_ram = !overlay && ram_pow2
                               && abs_addr < ram_size
                               && (abs_addr & 1) == 0;

            if (addr_in_ram) {
                emit_advance_flush(&e);
                emit_l32r_at(&e, 9, lit_off[ADDR_RAM_BASE],
                             entry_off + e.len);
                if ((i32)abs_addr >= -128 && (i32)abs_addr <= 127) {
                    xt_addi(&e, 9, 9, (i32)abs_addr);
                } else if ((i32)abs_addr >= -2048 && (i32)abs_addr <= 2047) {
                    xt_movi(&e, 10, (i32)abs_addr);
                    xt_add (&e, 9, 9, 10);
                } else {
                    emit_load_imm(&e, 10, 11, abs_addr);
                    xt_add (&e, 9, 9, 10);
                }
                xt_l8ui (&e, 10, 9, 0);            /* high byte */
                xt_l8ui (&e, 11, 9, 1);            /* low byte */
                xt_slli (&e, 10, 10, 8);
                xt_or   (&e, 10, 10, 11);          /* a10 = .W */

                /* Merge into Dn[15:0] preserving Dn[31:16]. Use the
                 * cache-slot-direct path when Dn is cached. */
                int dn_xt = cache_lookup(&rc, G_D(dn));
                u8 dn_reg = (dn_xt >= 0) ? (u8)dn_xt : 11;
                if (dn_xt < 0) emit_read_g(&e, &rc, G_D(dn), 11);
                xt_srli(&e, dn_reg, dn_reg, 16);
                xt_slli(&e, dn_reg, dn_reg, 16);
                xt_or  (&e, dn_reg, dn_reg, 10);
                if (dn_xt >= 0) {
                    for (int s = 0; s < rc.active; s++)
                        if (rc.guest[s] == (u8)G_D(dn)) { rc.dirty |= (u16)(1u << s); break; }
                } else {
                    emit_write_g(&e, &rc, G_D(dn), 11);
                }
                if (!flags_dead[i]) {
                    xt_slli(&e, 8, 10, 16);
                    emit_logic_flags(&e, 8);
                }
                emit_advance(&e, 4, 8);
                inline_ops++; done = true;
            }
            /* else: fall through to helper. */
        } else if (top == 0x1 && ((w >> 6) & 7) == 0 && mode == 7 && (w & 7) == 0) {
            /* M6.109 — MOVE.B (xxx).W,Dn. Bench-hot 0x1638 at 21 K helpers /
             * 100 M cyc. Compile-time RAM check; on RAM hit, read 1 byte
             * and merge into Dn[7:0] preserving Dn[31:8]. Length 4,
             * cycles 8. */
            int dn = (w >> 9) & 7;
            u16 ext = mac_read16(cpu->mem, op_pc[i] + 2);
            u32 abs_addr = (u32)(i32)(i16)ext;
            abs_addr &= 0xFFFFFFu;

            u32 ram_size = cpu->mem ? cpu->mem->ram_size : 0;
            bool overlay = cpu->mem ? cpu->mem->overlay : true;
            bool ram_pow2 = ram_size > 0 && (ram_size & (ram_size - 1)) == 0;
            bool addr_in_ram = !overlay && ram_pow2 && abs_addr < ram_size;
            /* MOVE.B has no alignment requirement (vs .W/.L). */

            if (addr_in_ram) {
                emit_advance_flush(&e);
                emit_l32r_at(&e, 9, lit_off[ADDR_RAM_BASE],
                             entry_off + e.len);
                if ((i32)abs_addr >= -128 && (i32)abs_addr <= 127) {
                    xt_addi(&e, 9, 9, (i32)abs_addr);
                } else if ((i32)abs_addr >= -2048 && (i32)abs_addr <= 2047) {
                    xt_movi(&e, 10, (i32)abs_addr);
                    xt_add (&e, 9, 9, 10);
                } else {
                    emit_load_imm(&e, 10, 11, abs_addr);
                    xt_add (&e, 9, 9, 10);
                }
                xt_l8ui (&e, 10, 9, 0);

                /* Merge byte into Dn[7:0] preserving Dn[31:8]. */
                int dn_xt = cache_lookup(&rc, G_D(dn));
                u8 dn_reg = (dn_xt >= 0) ? (u8)dn_xt : 11;
                if (dn_xt < 0) emit_read_g(&e, &rc, G_D(dn), 11);
                xt_srli(&e, dn_reg, dn_reg, 8);
                xt_slli(&e, dn_reg, dn_reg, 8);
                xt_or  (&e, dn_reg, dn_reg, 10);
                if (dn_xt >= 0) {
                    for (int s = 0; s < rc.active; s++)
                        if (rc.guest[s] == (u8)G_D(dn)) { rc.dirty |= (u16)(1u << s); break; }
                } else {
                    emit_write_g(&e, &rc, G_D(dn), 11);
                }
                if (!flags_dead[i]) {
                    xt_slli(&e, 8, 10, 24);
                    emit_logic_flags(&e, 8);
                }
                emit_advance(&e, 4, 8);
                inline_ops++; done = true;
            }
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
                   && mode == 7 && (w & 7) == 2) {
            /* M6.107 — LEA (d16,PC),An — bench-hot 0x41FA at 21 K helpers /
             * 100 M cyc (in the bench's post-cycle-11898 path). Target is
             * a compile-time constant: An = op_pc + 2 + sext16(d16).
             *
             * Same length (4) and cycles (8) as the other LEA forms.
             * Stash target in the literal pool when possible for a 1-op
             * l32r; otherwise emit_load_imm32 (10 ops). */
            int an = (w >> 9) & 7;
            u16 ext = mac_read16(cpu->mem, op_pc[i] + 2);
            u32 target = op_pc[i] + 2 + (u32)(i32)(i16)ext;

            if (g_pc_lit_valid && target == g_pc_lit_val) {
                emit_l32r_at(&e, 8, g_pc_lit_off,
                             g_pc_lit_entry_off + e.len);
            } else {
                emit_load_imm32(&e, 8, 9, target);
            }
            emit_write_g(&e, &rc, G_A(an), 8);
            emit_advance(&e, 4, 8);
            inline_ops++; done = true;
        } else if (top == 0x4 && ((w >> 6) & 7) == 7
                   && (mode == 2 || mode == 5 || mode == 6
                       || (mode == 7 && (w & 7) <= 1))) {
            /* LEA <ea>,An */
            int sr = w & 7;
            u16 ext   = (mode == 5 || mode == 6 || (mode == 7 && sr == 0))
                      ? mac_read16(cpu->mem, op_pc[i] + 2) : 0;
            u32 ext32 = (mode == 7 && sr == 1)
                      ? mac_read32(cpu->mem, op_pc[i] + 2) : 0;
            int am = (w >> 9) & 7;

            /* M6.85b — Fusion: LEA (d8,An,Xn.W),Am + ADDA.W Xn,Am.
             * Only the (d8,An,Xn) brief-format form with a .W index
             * matches — and Xn must equal the ADDA's source Dn|An. */
            bool lea_fused = false;
            if (mode == 6 && (ext & 0x0800) == 0      /* .W index */
                && (ext & 0x0100) == 0                /* brief format */
                && i + 1 < n_ops) {
                u16 nw = op_word[i + 1];
                if (((nw >> 12) & 0xF) == 0xD          /* ADD/ADDA */
                    && ((nw >> 6) & 7) == 3            /* ADDA.W */
                    && ((nw >> 3) & 7) <= 1            /* src is Dn or An */
                    && ((nw >> 9) & 7) == am) {        /* same dst */
                    int adda_src    = nw & 7;
                    bool adda_is_an = ((nw >> 3) & 7) == 1;
                    int lea_idx     = (ext >> 12) & 7;
                    bool lea_idx_an = (ext & 0x8000) != 0;
                    if (adda_src == lea_idx && adda_is_an == lea_idx_an) {
                        int d8 = (i8)(ext & 0xFF);
                        emit_lea_adda_fused(&e, am, sr,
                                            lea_idx, lea_idx_an, d8, &rc);
                        i++;                  /* skip absorbed ADDA */
                        lea_fused = true;
                    }
                }
            }
            if (!lea_fused) {
                emit_lea(&e, am, mode, sr, ext, ext32, &rc);
            }
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
        } else if (top == 0x6 && (((w>>8)&0xF) != 1) && (w & 0xFF) == 0x00) {
            /* M6.106 — BRA.W / Bcc.W disp16. Block terminator with
             * 16-bit displacement. Same chain-preservation lever as
             * M6.105 BSR.W — inline keeps the JIT chain unbroken
             * through conditional/unconditional branches with
             * displacements beyond ±128 bytes.
             *
             * Length 4 (op + disp16). Cycles in interp: 10 if taken/BRA,
             * 8 if not taken — same as Bcc.S in our model. */
            int cc = (w >> 8) & 0xF;
            u16 ext = mac_read16(cpu->mem, op_pc[i] + 2);
            i32 disp = (i16)ext;
            u32 ft    = op_pc[i] + 4;
            u32 taken = op_pc[i] + 2 + (u32)disp;

            i32 extra_cyc = g_cyc_acc;
            g_pc_acc = 0;
            g_cyc_acc = 0;

            if (cc == 0) {
                /* BRA.W — unconditional. */
                if (g_pc_lit_valid && taken == g_pc_lit_val) {
                    emit_l32r_at(&e, 8, g_pc_lit_off, g_pc_lit_entry_off + e.len);
                } else {
                    emit_load_imm32(&e, 8, 9, taken);
                }
                xt_s32i(&e, 8, R_CPU, OFF_PC);
                xt_l32i(&e, 8, R_CPU, OFF_CYCLES);
                i32 total = 14 + extra_cyc;
                if (total >= -128 && total <= 127) {
                    xt_addi(&e, 8, 8, total);
                } else {
                    emit_load_imm(&e, 9, 10, (u32)total);
                    xt_add(&e, 8, 8, 9);
                }
                xt_s32i(&e, 8, R_CPU, OFF_CYCLES);
            } else {
                /* Bcc.W — conditional. emit_bcc_branchless_tail computes
                 * `taken = ft + disp`. For Bcc.W, ft = op_pc + 4 but the
                 * disp16 is relative to op_pc + 2 (m68k semantic). To
                 * make the tail's `ft + disp_for_tail = taken` work,
                 * pass `disp - 2`. Same adjustment as the M6.38 DBcc arm
                 * which has the same 4-byte op-length quirk. */
                emit_cond(&e, cc);
                emit_bcc_branchless_tail(&e, ft, disp - 2, 12 + extra_cyc);
            }
            sext_memo_invalidate();
            inline_ops++; done = true;
        } else if (top == 0x6 && (((w>>8)&0xF) == 1)
                   && (w & 0xFF) != 0x00 && (w & 0xFF) != 0xFF) {
            /* BSR.S disp8 — bench-warm 0x61D2 at 193 hits/20 M cyc (M6.83).
             * Block terminator. Sibling of M6.78's JSR (d16,PC) but with
             * 8-bit displacement (length 2). The target is
             * `source_pc + 2 + sext8(disp8)` — compile-time constant,
             * stashed in LITERAL_BCC_PC by the block-setup pre-pass
             * (extended to handle cc=1 this iteration). Return PC =
             * source_pc + 2 (computed at runtime as cpu->pc + 2 since
             * emit_advance_flush just landed cpu->pc on source_pc).
             *
             * 22 cycles (interp base 4 + handler 18). */
            i32 disp = (i8)(w & 0xFF);
            u32 target_pc = op_pc[i] + 2 + (u32)disp;

            emit_advance_flush(&e);
            emit_read_g(&e, &rc, G_A(7), 8);
            xt_addi(&e, 8, 8, -4);                /* a8 = new SP */
            emit_l32r_at(&e, 9, lit_off[LITERAL_RAM_BOUNDS],
                         entry_off + e.len);
            xt_and  (&e, 10, 8, 9);
            emit_cache_flush(&e, &rc);
            i32 op_pc_bsr = 0;        /* m68k_step sets cpu->pc directly */
            i32 op_cyc_bsr = 22;
            xt_beqz (&e, 10, (i32)(6u + helper_step_after_flush_undo_size(&rc, op_pc_bsr, op_cyc_bsr)));
            emit_helper_step_after_flush_undo(&e, lit_off[HELPER_M68K_STEP],
                                              entry_off, &rc, op_pc_bsr, op_cyc_bsr);
            u32 jbsr_pos = e.len;
            xt_j    (&e, 4);
            /* Fast path: write return_pc (= cpu->pc + 2) as 4 BE bytes
             * to [ram_base + new SP], commit new SP, set cpu->pc =
             * target. */
            xt_l32i (&e, 10, R_CPU, OFF_PC);
            xt_addi (&e, 10, 10, 2);              /* a10 = return_pc */
            emit_l32r_at(&e, 9, lit_off[ADDR_RAM_BASE],
                         entry_off + e.len);
            xt_add  (&e, 9, 9, 8);
            xt_extui(&e, 11, 10, 24, 7); xt_s8i(&e, 11, 9, 0);
            xt_extui(&e, 11, 10, 16, 7); xt_s8i(&e, 11, 9, 1);
            xt_extui(&e, 11, 10,  8, 7); xt_s8i(&e, 11, 9, 2);
            xt_extui(&e, 11, 10,  0, 7); xt_s8i(&e, 11, 9, 3);
            emit_write_g(&e, &rc, G_A(7), 8);
            /* Set cpu->pc = target. LITERAL_BCC_PC holds it. */
            if (g_pc_lit_valid && target_pc == g_pc_lit_val) {
                emit_l32r_at(&e, 10, g_pc_lit_off,
                             g_pc_lit_entry_off + e.len);
            } else {
                emit_load_imm32(&e, 10, 11, target_pc);
            }
            xt_s32i(&e, 10, R_CPU, OFF_PC);
            emit_advance(&e, op_pc_bsr, op_cyc_bsr);

            u32 here_bsr = e.len;
            i32 jo_bsr = (i32)(here_bsr - jbsr_pos) - 4;
            u32 jw_bsr = ((u32)((u32)jo_bsr & 0x3FFFFu) << 6) | 0x06u;
            base[entry_off + jbsr_pos    ] = (u8)jw_bsr;
            base[entry_off + jbsr_pos + 1] = (u8)(jw_bsr >> 8);
            base[entry_off + jbsr_pos + 2] = (u8)(jw_bsr >> 16);

            sext_memo_invalidate();
            inline_ops++; done = true;
        } else if (top == 0x6 && (((w >> 8) & 0xF) == 1) && (w & 0xFF) == 0) {
            /* M6.105 — BSR.W disp16. Bench-warm 0x6100 (BSR.W with
             * disp16) at 117 helpers / 20 M cyc. Block terminator —
             * inlining keeps the JIT chain unbroken through subroutine
             * calls, matching the same chain-preservation insight that
             * drove M6.102's DBEQ win.
             *
             * Same structure as BSR.S but the 16-bit displacement is in
             * the ext word; length is 4 bytes (op + disp16). The target
             * is op_pc + 2 + sext16(disp16), stashed in LITERAL_BCC_PC
             * by the block-setup pre-pass extended above.
             *
             * Cycles: 22 (m68k_step base 4 + handler 18, same as BSR.S). */
            u16 ext = mac_read16(cpu->mem, op_pc[i] + 2);
            i32 disp16 = (i16)ext;
            u32 target_pc = op_pc[i] + 2 + (u32)disp16;

            emit_advance_flush(&e);
            emit_read_g(&e, &rc, G_A(7), 8);
            xt_addi(&e, 8, 8, -4);
            emit_l32r_at(&e, 9, lit_off[LITERAL_RAM_BOUNDS],
                         entry_off + e.len);
            xt_and  (&e, 10, 8, 9);
            emit_cache_flush(&e, &rc);
            i32 op_pc_bsw = 0;        /* m68k_step sets cpu->pc directly */
            i32 op_cyc_bsw = 22;
            xt_beqz (&e, 10, (i32)(6u + helper_step_after_flush_undo_size(&rc, op_pc_bsw, op_cyc_bsw)));
            emit_helper_step_after_flush_undo(&e, lit_off[HELPER_M68K_STEP],
                                              entry_off, &rc, op_pc_bsw, op_cyc_bsw);
            u32 jbsw_pos = e.len;
            xt_j    (&e, 4);
            /* Fast path: write return PC (= cpu->pc + 4 since BSR.W is 4
             * bytes), commit new SP, set cpu->pc = target. */
            xt_l32i (&e, 10, R_CPU, OFF_PC);
            xt_addi (&e, 10, 10, 4);
            emit_l32r_at(&e, 9, lit_off[ADDR_RAM_BASE],
                         entry_off + e.len);
            xt_add  (&e, 9, 9, 8);
            xt_extui(&e, 11, 10, 24, 7); xt_s8i(&e, 11, 9, 0);
            xt_extui(&e, 11, 10, 16, 7); xt_s8i(&e, 11, 9, 1);
            xt_extui(&e, 11, 10,  8, 7); xt_s8i(&e, 11, 9, 2);
            xt_extui(&e, 11, 10,  0, 7); xt_s8i(&e, 11, 9, 3);
            emit_write_g(&e, &rc, G_A(7), 8);
            if (g_pc_lit_valid && target_pc == g_pc_lit_val) {
                emit_l32r_at(&e, 10, g_pc_lit_off,
                             g_pc_lit_entry_off + e.len);
            } else {
                emit_load_imm32(&e, 10, 11, target_pc);
            }
            xt_s32i(&e, 10, R_CPU, OFF_PC);
            emit_advance(&e, op_pc_bsw, op_cyc_bsw);

            u32 here_bsw = e.len;
            i32 jo_bsw = (i32)(here_bsw - jbsw_pos) - 4;
            u32 jw_bsw = ((u32)((u32)jo_bsw & 0x3FFFFu) << 6) | 0x06u;
            base[entry_off + jbsw_pos    ] = (u8)jw_bsw;
            base[entry_off + jbsw_pos + 1] = (u8)(jw_bsw >> 8);
            base[entry_off + jbsw_pos + 2] = (u8)(jw_bsw >> 16);

            sext_memo_invalidate();
            inline_ops++; done = true;
        } else if (top == 0x5 && szf == 3 && mode == 1
                   && (((w >> 8) & 0xF) == 1 || ((w >> 8) & 0xF) == 6
                       || ((w >> 8) & 0xF) == 7)) {
            /* DBF / DBNE / DBEQ Dn, disp16 — bench/boot DBcc cases.
             *   DBF (cc=1):   always decrement+branch.
             *   DBNE (cc=6):  if Z==0 (NE true), fall through.
             *                 Else (Z==1), decrement+branch.
             *   DBEQ (cc=7):  if Z==1 (EQ true), fall through.
             *                 Else (Z==0), decrement+branch.
             *
             *   if !cond_true: Dn.W = Dn.W - 1;
             *                  pc = (Dn.W != -1) ? op_pc+2+disp16 : op_pc+4;
             *   else:          pc = op_pc + 4;
             *   cycles += 14 (unconditional).
             *
             * DBEQ added in M6.102: bench-warm 0x57CD (DBEQ D5,disp16) at
             * 236 helpers / 20 M cyc. The "Z test direction" differs from
             * DBNE — DBEQ exits when Z=1, DBNE exits when Z=0. Just flips
             * a bnez↔beqz in the dec-skip and cond-compute logic. */
            int cc = (w >> 8) & 0xF;
            int dn = w & 7;
            i16 disp16 = (i16)mac_read16(cpu->mem, op_pc[i] + 2);
            u32 ft = op_pc[i] + 4;
            i32 disp_for_tail = (i32)disp16 - 2;
            bool is_dbne = (cc == 6);
            bool is_dbeq = (cc == 7);
            bool has_z_test = is_dbne || is_dbeq;

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
            /* 3. For DBNE/DBEQ: if exit-condition true, restore a10 = Dn_old
             *    to skip the decrement.
             *    DBNE exits on Z=0; DBEQ exits on Z=1. */
            if (has_z_test) {
                xt_extui(&e, 13, R_SR, 2, 0);   /* a13 = Z (0 or 1) */
                if (is_dbne) {
                    xt_bnez (&e, 13, 6);        /* Z != 0 (NE false): keep decremented */
                } else {  /* DBEQ */
                    xt_beqz (&e, 13, 6);        /* Z == 0 (EQ false): keep decremented */
                }
                xt_mov  (&e, 10, 11);           /* exit-true: restore old (no dec) */
            }
            /* 4. Write Dn from a10. */
            emit_write_g(&e, &rc, G_D(dn), 10);
            /* 5. Cond_branch compute:
             *    DBF: cond = (old_dn.W != 0) ? 1 : 0.
             *    DBNE: cond = (Z=1, i.e. NE false) AND (old_dn.W != 0).
             *    DBEQ: cond = (Z=0, i.e. EQ false) AND (old_dn.W != 0). */
            xt_extui(&e, 12, 11, 0, 15);        /* a12 = old_dn.W */
            if (has_z_test) {
                /* a13 still holds Z from step 3. */
                xt_movi(&e, 8, 0);              /* default cond = 0 */
                if (is_dbne) {
                    xt_beqz(&e, 13, 12);        /* Z==0 → cond stays 0 */
                } else {  /* DBEQ */
                    xt_bnez(&e, 13, 12);        /* Z!=0 → cond stays 0 */
                }
                xt_movi(&e, 8, 1);              /* assume cond = 1 */
                xt_bnez(&e, 12, 6);             /* old_dn.W != 0 → keep cond=1 */
                xt_movi(&e, 8, 0);              /* old_dn.W == 0 → cond = 0 */
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
        } else if ((top == 0xD || top == 0x9) && (szf == 0 || szf == 1)
                   && !((w >> 8) & 1) && mode == 0) {
            /* ADD.B/W / SUB.B/W Dm,Dn — boot-warm 0xD441 (~9 K) for .W;
             * M6.112 extends to .B (bench-hot 0xD603 = ADD.B D3,D3 at
             * 21 K helpers / 100 M cyc on the post-cycle-11898 path).
             *
             * size_bits = 8 for .B, 16 for .W. The "shift to high"
             * trick puts the size's sign bit at register bit 31 so
             * emit_addsub_flags_long reads it directly. Shift amount
             * is (32 - size_bits) = 24 for .B, 16 for .W. */
            bool is_sub = (top == 0x9);
            int dn = (w >> 9) & 7;
            int dm = w & 7;
            int size_bits = (szf == 0) ? 8 : 16;
            int up = 32 - size_bits;
            emit_read_g(&e, &rc, G_D(dm), 8);
            emit_read_g(&e, &rc, G_D(dn), 11);
            xt_slli(&e, 8, 8, up);
            xt_slli(&e, 9, 11, up);
            if (is_sub) xt_sub(&e, 10, 9, 8);
            else        xt_add(&e, 10, 9, 8);
            xt_srli(&e, 11, 11, size_bits);
            xt_slli(&e, 11, 11, size_bits);
            xt_extui(&e, 12, 10, up, size_bits - 1);
            xt_or  (&e, 11, 11, 12);
            emit_write_g(&e, &rc, G_D(dn), 11);
            if (!flags_dead[i]) {
                emit_addsub_flags_long(&e, is_sub, false);
            }
            emit_advance(&e, 2, 8);   /* m68k_step base 4 + handler 4 = 8 */
            inline_ops++; done = true;
        } else if ((top == 0xD || top == 0x9) && szf == 1
                   && !((w >> 8) & 1) && mode == 5) {
            /* ADD.W / SUB.W (d16,An),Dn — bench-warm 0xD06D (ADD.W
             * (d16,A5),D0) at 515 hits/20 M cyc (M6.80). Sibling of the
             * ADD.W Dm,Dn arm above but the source is a .W memory read
             * via (d16,An). Same "shift to high 16" flag trick to get
             * size-correct N/Z/V/C/X from the 32-bit subtract/add.
             * Source can be ROM, so use the M6.76 unified bounds + base
             * selector. Length 4, 8 cycles. */
            bool is_sub = (top == 0x9);
            int dn = (w >> 9) & 7;
            int src_an = w & 7;
            u16 ext = mac_read16(cpu->mem, op_pc[i] + 2);
            i32 d16 = (i16)ext;

            emit_advance_flush(&e);
            emit_read_g(&e, &rc, G_A(src_an), 8);
            if (d16 >= -128 && d16 <= 127) {
                xt_addi(&e, 8, 8, d16);
            } else {
                emit_load_imm(&e, 11, 12, (u32)d16);
                xt_add (&e, 8, 8, 11);
            }
            /* Unified RAM-or-ROM source bounds (M6.76). */
            emit_l32r_at(&e, 9, lit_off[LITERAL_RAM_BOUNDS],
                         entry_off + e.len);
            xt_and  (&e, 10, 8, 9);
            emit_l32r_at(&e, 9, lit_off[LITERAL_ROM_BOUNDS],
                         entry_off + e.len);
            xt_and  (&e, 11, 8, 9);
            emit_l32r_at(&e, 9, lit_off[LITERAL_ROM_BASE],
                         entry_off + e.len);
            xt_xor  (&e, 11, 11, 9);
            xt_and  (&e, 12, 10, 11);
            emit_cache_flush(&e, &rc);
            i32 op_pc_awd = 4, op_cyc_awd = 8;
            xt_beqz (&e, 12, (i32)(6u + helper_step_after_flush_undo_size(&rc, op_pc_awd, op_cyc_awd)));
            emit_helper_step_after_flush_undo(&e, lit_off[HELPER_M68K_STEP],
                                              entry_off, &rc, op_pc_awd, op_cyc_awd);
            u32 jawd_pos = e.len;
            xt_j    (&e, 4);
            /* Fast path: pick base via a10. */
            emit_l32r_at(&e, 9, lit_off[ADDR_RAM_BASE],
                         entry_off + e.len);
            xt_beqz (&e, 10, 6);
            emit_l32r_at(&e, 9, lit_off[ADDR_ROM_HOST_BASE],
                         entry_off + e.len);
            xt_add  (&e, 9, 9, 8);
            /* Read .W → a11. */
            xt_l8ui (&e, 11, 9, 0);
            xt_l8ui (&e, 12, 9, 1);
            xt_slli (&e, 11, 11, 8);
            xt_or   (&e, 11, 11, 12);
            /* s = mem.W shifted to high 16 → a8. */
            xt_slli (&e, 8, 11, 16);
            /* d = Dn.W shifted to high 16 → a9; also keep full Dn in a11
             * for the high-16 preserve. */
            emit_read_g(&e, &rc, G_D(dn), 11);
            xt_slli (&e, 9, 11, 16);
            /* r = d ± s → a10. */
            if (is_sub) xt_sub(&e, 10, 9, 8);
            else        xt_add(&e, 10, 9, 8);
            /* Write back: Dn[31:16] preserved, Dn[15:0] = bits 16..31 of r. */
            xt_srli (&e, 11, 11, 16);
            xt_slli (&e, 11, 11, 16);
            xt_extui(&e, 12, 10, 16, 15);
            xt_or   (&e, 11, 11, 12);
            emit_write_g(&e, &rc, G_D(dn), 11);
            if (!flags_dead[i]) {
                emit_addsub_flags_long(&e, is_sub, false);
            }
            emit_advance(&e, op_pc_awd, op_cyc_awd);

            u32 here_awd = e.len;
            i32 jo_awd = (i32)(here_awd - jawd_pos) - 4;
            u32 jw_awd = ((u32)((u32)jo_awd & 0x3FFFFu) << 6) | 0x06u;
            base[entry_off + jawd_pos    ] = (u8)jw_awd;
            base[entry_off + jawd_pos + 1] = (u8)(jw_awd >> 8);
            base[entry_off + jawd_pos + 2] = (u8)(jw_awd >> 16);

            inline_ops++; done = true;
        } else if (top == 0x4 && ((w >> 8) & 0xF) == 0xA && szf == 0 && mode == 0) {
            /* M6.101 — TST.B Dn — bench-warm 0x4A05 (TST.B D5) at 191
             * helpers / 20 M cyc. N from bit 7, Z from byte == 0,
             * V=C=0, X preserved. Length 2, 4 cycles. */
            int dn = w & 7;
            u8 dn_reg = emit_read_g_in(&e, &rc, G_D(dn), 9);
            if (!flags_dead[i]) {
                /* Shift Dn.B's bit 7 to bit 31 for emit_logic_flags. */
                xt_slli(&e, 8, dn_reg, 24);
                emit_logic_flags(&e, 8);
            }
            emit_advance(&e, 2, 4);
            inline_ops++; done = true;
        } else if (top == 0x4 && ((w >> 8) & 0xF) == 0xA && szf == 2 && mode == 0) {
            /* TST.L Dn — bench-warm 0x4A80/0x4A81 (~4 K). N/Z from Dn,
             * V=C=0, X preserved.
             *
             * M6.95 — TST+Bcc fusion: when followed by Bcc.S
             * cc∈{6,7,13,15} (NE/EQ/BLT/BLE), skip the R_SR write and
             * compute the condition directly from Dn. Reuses
             * `emit_cmp_cond_fused` by passing s=d=r=Dn: with s==d the
             * V term `(s^d)&(d^r)` collapses to 0, leaving N=bit31(r)
             * and Z=(r==0) — exactly TST's CCR convention. Saves the
             * ~8-op emit_logic_flags + the ~3-op emit_cond's R_SR read
             * per TST+Bcc fast-path call. */
            int dn = w & 7;

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

            /* emit_bcc_branchless_tail OVERWRITES cpu->pc directly — any
             * compile-time-accumulated g_pc_acc / g_cyc_acc from prior
             * inline ops would be lost. Flush them to memory first when
             * we're going to fuse. (The non-fused path keeps the deferred-
             * accumulator pattern via emit_advance.) */
            if (fuse) emit_advance_flush(&e);

            /* Use emit_read_g_in so a cached Dn is read in-place; the
             * scratch register is a9, NOT a8 — emit_cmp_cond_fused writes
             * cond into a8, so we must not pass r=a8 (the bnez in cc=6/7
             * would self-read the just-overwritten value). */
            u8 dn_reg = emit_read_g_in(&e, &rc, G_D(dn), 9);

            if (fuse) {
                emit_cmp_cond_fused(&e, fuse_cc, dn_reg, dn_reg, dn_reg);
                /* TST.L base 4 + Bcc.S base 12 = 16 cycles folded. */
                emit_bcc_branchless_tail(&e, fuse_ft, fuse_disp, 16);
                g_pc_acc = 0;
                g_cyc_acc = 0;
                i++;   /* skip the absorbed Bcc.S */
            } else {
                if (!flags_dead[i]) emit_logic_flags(&e, dn_reg);
                emit_advance(&e, 2, 4);
            }
            inline_ops++; done = true;
        } else if ((w & 0xFFF8) == 0x4840) {
            /* SWAP Dn — bench-warm 0x4840+ at ~246 hits/20 M cyc (M6.83).
             * Swap the high and low .W halves of Dn: result = (v >> 16) |
             * (v << 16). MOVE-family CCR (N from bit 31, Z if result==0,
             * V=C=0, X preserved). Length 2, 4 cycles (interp returns
             * after set_flags_logic — no `cycles +=` after the base 4).
             *
             * Uses `xt_extui` for both halves since `xt_srli`'s shift
             * count is capped at 15 — we'd otherwise need two shifts to
             * land bit-16-and-above in the low 16. */
            int dn = w & 7;
            emit_read_g(&e, &rc, G_D(dn), 8);
            xt_extui(&e, 9, 8, 16, 15);             /* a9 = v[31:16] in low 16 */
            xt_slli (&e, 10, 8, 16);                /* a10 = v[15:0] in high 16 */
            xt_or   (&e, 8, 10, 9);                  /* a8 = swapped */
            emit_write_g(&e, &rc, G_D(dn), 8);
            if (!flags_dead[i]) emit_logic_flags(&e, 8);
            emit_advance(&e, 2, 4);
            inline_ops++; done = true;
        } else if ((w & 0xFFF8) == 0x48C0) {
            /* M6.99 — EXT.L Dn — boot-warm 0x48C1 (EXT.L D1) at ~600
             * helpers / 100 M cyc. Sign-extend Dn[15:0] to Dn[31:0]:
             * bit 15 propagates into bits 16-31.
             *
             * Implementation: xt_slli 16; xt_srai 16. Two ops total.
             * Replaces the full 32-bit Dn (no merge needed). Flags are
             * the classic MOVE-family: N=bit31(result), Z=(result==0),
             * V=C=0, X preserved. Length 2, 4 cycles. */
            int dn = w & 7;
            int dn_xt = cache_lookup(&rc, G_D(dn));
            u8 src_reg = (dn_xt >= 0) ? (u8)dn_xt : 8;
            if (dn_xt < 0) emit_read_g(&e, &rc, G_D(dn), 8);
            u8 dst_reg = (dn_xt >= 0) ? (u8)dn_xt : 8;
            xt_slli(&e, dst_reg, src_reg, 16);          /* .W at bit 31 */
            xt_srai(&e, dst_reg, dst_reg, 16);          /* arith shift back */
            if (dn_xt >= 0) {
                for (int s = 0; s < rc.active; s++)
                    if (rc.guest[s] == (u8)G_D(dn)) { rc.dirty |= (u16)(1u << s); break; }
            } else {
                emit_write_g(&e, &rc, G_D(dn), 8);
            }
            if (!flags_dead[i]) emit_logic_flags(&e, dst_reg);
            emit_advance(&e, 2, 4);
            inline_ops++; done = true;
        } else if ((w & 0xFFF8) == 0x4880) {
            /* M6.99 — EXT.W Dn — sign-extend Dn[7:0] to Dn[15:0],
             * preserving Dn[31:16]. Less common than EXT.L but cheap
             * to add alongside.
             *
             * Implementation:
             *   xt_slli a9, Dn, 24       ; bit 7 at bit 31
             *   xt_srai a9, a9, 24       ; arith shift; sign-ext .B to .L
             *   xt_extui a9, a9, 0, 15   ; mask to 16 bits (= .W result)
             *   xt_extui a11, Dn, 16, 15 ; Dn.high_16
             *   xt_slli  a11, a11, 16
             *   xt_or    Dn, a11, a9     ; merge
             *
             * Flag bit 15 of result is N (sign of new .W); shift to bit 31
             * for emit_logic_flags. Z = (result.W == 0). V=C=0, X preserved.
             * Length 2, 4 cycles. */
            int dn = w & 7;
            int dn_xt = cache_lookup(&rc, G_D(dn));
            u8 src_reg = (dn_xt >= 0) ? (u8)dn_xt : 8;
            if (dn_xt < 0) emit_read_g(&e, &rc, G_D(dn), 8);

            xt_slli (&e, 9, src_reg, 24);
            xt_srai (&e, 9, 9, 24);                      /* sign-ext .B to 32 */
            xt_extui(&e, 9, 9, 0, 15);                   /* low 16 = result.W */
            xt_extui(&e, 11, src_reg, 16, 15);            /* Dn.high_16 */
            xt_slli (&e, 11, 11, 16);
            u8 dst_reg = (dn_xt >= 0) ? (u8)dn_xt : 8;
            xt_or   (&e, dst_reg, 11, 9);

            if (dn_xt >= 0) {
                for (int s = 0; s < rc.active; s++)
                    if (rc.guest[s] == (u8)G_D(dn)) { rc.dirty |= (u16)(1u << s); break; }
            } else {
                emit_write_g(&e, &rc, G_D(dn), 8);
            }
            if (!flags_dead[i]) {
                /* For .W: N = bit 15. Shift to bit 31 for emit_logic_flags. */
                xt_slli (&e, 8, 9, 16);
                emit_logic_flags(&e, 8);
            }
            emit_advance(&e, 2, 4);
            inline_ops++; done = true;
        } else if (top == 0x4 && ((w >> 8) & 0xF) == 0xA && szf == 0
                   && mode == 7 && (w & 7) == 0) {
            /* TST.B (xxx).W — bench-hot 0x4A38 at ~15 K helpers in 20 M cyc
             * (M6.77). The 16-bit ext word holds a signed absolute address
             * (range ±32 K), known at COMPILE TIME. For Mac Plus that's
             * either low-memory RAM globals (0x000000-0x007FFE) or the
             * high MMIO region (0xFF8000-0xFFFFFE). We inline only when
             * compile-time analysis places the address in valid RAM —
             * the MMIO case still needs m68k_step to dispatch through
             * region_of() and the device handlers.
             *
             * Bit-field decode (per memory/triple-differential.md):
             *   0x4A38 = 0100_1010_0011_1000
             *            top=4 op=A   sz=00 m=111 reg=000
             *          → top=0x4, op11-8=0xA (TST), size=byte, mode=7/reg=0 = (xxx).W
             *
             * No runtime bounds check needed — the static check is exact.
             * Fast path: load RAM_BASE + abs_addr → host ptr; l8ui byte;
             * sext to 32 via `<< 24` so emit_logic_flags reads bit 31 as
             * the sign of the byte and Z is preserved (shift by 24 keeps
             * zero-ness). MOVE-family CCR with V=C=0, X kept. 4 cycles
             * (interp returns after set_flags_logic, no `cycles +=` after
             * the base 4). */
            u16 ext = mac_read16(cpu->mem, op_pc[i] + 2);
            u32 abs_addr = (u32)(i32)(i16)ext;
            abs_addr &= 0xFFFFFFu;             /* mac_mem's 24-bit mask */

            u32 ram_size = cpu->mem ? cpu->mem->ram_size : 0;
            bool overlay = cpu->mem ? cpu->mem->overlay : true;
            bool ram_pow2 = ram_size > 0 && (ram_size & (ram_size - 1)) == 0;
            bool addr_in_ram = !overlay && ram_pow2 && abs_addr < ram_size;

            if (addr_in_ram) {
                /* M6.95 — TST.B + Bcc.S fusion (sibling of M6.95 TST.L+Bcc).
                 * The byte's bit 7 becomes bit 31 after shift, so passing
                 * s == d == r == shifted_byte to emit_cmp_cond_fused makes
                 * V cancel (s==d) and N=bit31(r)=byte sign, Z=(byte==0). */
                bool fuse_tb = false;
                int fuse_tb_cc = 0;
                i32 fuse_tb_disp = 0;
                u32 fuse_tb_ft = 0;
                if (i + 1 < n_ops) {
                    u16 nw = op_word[i + 1];
                    if ((nw >> 12) == 0x6) {
                        int cc = (nw >> 8) & 0xF;
                        i32 disp = (i8)(nw & 0xFF);
                        if ((cc == 6 || cc == 7 || cc == 13 || cc == 15) && disp != 0 && disp != -1) {
                            fuse_tb = true;
                            fuse_tb_cc = cc;
                            fuse_tb_disp = disp;
                            fuse_tb_ft = op_pc[i + 1] + 2;
                        }
                    }
                }

                emit_advance_flush(&e);
                emit_l32r_at(&e, 9, lit_off[ADDR_RAM_BASE],
                             entry_off + e.len);
                /* Add abs_addr to a9. */
                if ((i32)abs_addr >= -128 && (i32)abs_addr <= 127) {
                    xt_addi(&e, 9, 9, (i32)abs_addr);
                } else if ((i32)abs_addr >= -2048 && (i32)abs_addr <= 2047) {
                    xt_movi(&e, 10, (i32)abs_addr);
                    xt_add (&e, 9, 9, 10);
                } else {
                    emit_load_imm(&e, 10, 11, abs_addr);
                    xt_add (&e, 9, 9, 10);
                }
                xt_l8ui(&e, 10, 9, 0);          /* a10 = byte */
                if (fuse_tb) {
                    /* Shift byte to bit 31 so N = byte's sign. */
                    xt_slli(&e, 10, 10, 24);
                    emit_cmp_cond_fused(&e, fuse_tb_cc, 10, 10, 10);
                    /* TST.B base 4 + Bcc.S base 12 = 16 cyc folded. */
                    emit_bcc_branchless_tail(&e, fuse_tb_ft, fuse_tb_disp, 16);
                    g_pc_acc = 0;
                    g_cyc_acc = 0;
                    i++;   /* skip absorbed Bcc.S */
                } else if (!flags_dead[i]) {
                    /* Shift byte to bit 31 so emit_logic_flags sees the
                     * .B sign bit at the same position it would for a 32-
                     * bit value, and Z is preserved (zero-shifted-zero). */
                    xt_slli(&e, 10, 10, 24);
                    emit_logic_flags(&e, 10);
                    emit_advance(&e, 4, 4);          /* len 4 (op+ext), 4 cyc */
                } else {
                    emit_advance(&e, 4, 4);
                }
                inline_ops++; done = true;
            }
            /* else: fall through to the generic helper-step path below. */
        } else if (top == 0x4 && ((w >> 8) & 0xF) == 0x4 && szf == 2 && mode == 0) {
            /* NEG.L Dn — bench-warm 0x4480-0x4487 at ~1500 helpers in 20M cyc
             * (M6.73). r = 0 - Dn; full CCR via the existing SUB-style flag
             * helper with s=Dn (operand being subtracted) and d=0. The
             * interp's set_flags_sub(sz, d, 0, r) matches this register
             * order. */
            int dn = w & 7;
            emit_read_g(&e, &rc, G_D(dn), 8);      /* a8 = s = Dn */
            xt_movi(&e, 9, 0);                      /* a9 = d = 0 */
            xt_sub (&e, 10, 9, 8);                  /* a10 = r = 0 - Dn */
            emit_write_g(&e, &rc, G_D(dn), 10);
            if (!flags_dead[i]) emit_addsub_flags_long(&e, true, false);
            emit_advance(&e, 2, 8);                 /* base 4 + handler 4 */
            inline_ops++; done = true;
        } else if (top == 0xB && szf == 2 && !((w >> 8) & 1) && mode == 0) {
            /* CMP.L Dm,Dn */
            emit_cmp_l_dd(&e, (w >> 9) & 7, w & 7, &rc, flags_needed[i]);
            inline_ops++; done = true;
        } else if (top == 0x9 && szf == 2 && !((w >> 8) & 1) && (mode == 0 || mode == 1)) {
            /* SUB.L Dm/Am,Dn — M6.104 extends to An source (bench 0x948A). */
            emit_sub_l_dd(&e, (w >> 9) & 7, w & 7, mode == 1, flags_dead[i], &rc);
            inline_ops++; done = true;
        } else if (top == 0xC && szf == 3
                   && mode == 7 && (w & 7) == 4) {
            /* MULS.W / MULU.W #imm16, Dn — bench-hot at
             *   0xC0FC = MULU.W #imm16, D0 (bit 8 = 0)  at 1002 hits
             *   0xC1FC = MULS.W #imm16, D0 (bit 8 = 1)  much rarer
             * per the M6.81 helper histo on the corrected path.
             *
             * Uses the new xt_mull encoder (added this iteration).
             * MULS sign-extends both operands to 32 bits before the
             * MULL; MULU zero-extends. Either way, the LOW 32 bits of
             * the product are correct for the destination Dn (full .L
             * write). MOVE-family CCR. Length 4 (op + imm), 74 cycles
             * (interp base 4 + handler 70 — the 68000's notoriously
             * slow MUL).
             *
             * Bit-field decode lesson (per memory/triple-differential.md):
             *   0xC0FC = 1100_0000_1111_1100  (bit 8 = 0 → MULU!)
             *   0xC1ED = 1100_0001_1110_1101  (bit 8 = 1 → MULS)
             * First M6.81 cut required `(w>>8)&1` and missed 0xC0FC
             * entirely — the helper histo still showed 1002 hits at
             * that opcode after the "inline". */
            bool sgn = (w >> 8) & 1;
            int dn = (w >> 9) & 7;
            u16 imm = mac_read16(cpu->mem, op_pc[i] + 2);
            u32 imm_ext = sgn ? (u32)(i32)(i16)imm : (u32)imm;

            emit_read_g(&e, &rc, G_D(dn), 8);
            if (sgn) {
                xt_slli (&e, 9, 8, 16);
                xt_srai (&e, 9, 9, 16);          /* a9 = sext(Dn.W) */
            } else {
                xt_extui(&e, 9, 8, 0, 15);        /* a9 = zext(Dn.W) */
            }
            emit_load_imm(&e, 10, 11, imm_ext);
            xt_mull (&e, 11, 9, 10);              /* a11 = .L product */
            emit_write_g(&e, &rc, G_D(dn), 11);
            if (!flags_dead[i]) emit_logic_flags(&e, 11);
            emit_advance(&e, 4, 74);
            inline_ops++; done = true;
        } else if (top == 0xC && szf == 3 && mode == 5) {
            /* MULS.W / MULU.W (d16,An), Dn — bench-hot 0xC1ED (MULS.W
             * (d16,A5),D0) at 1000 hits/20 M cyc (M6.81). Sibling of the
             * #imm16 arm above but src is a .W memory read. Source can
             * be in ROM, so use the M6.76 unified RAM/ROM bounds + base
             * selector. Length 4 (op + d16), 74 cycles. */
            bool sgn = (w >> 8) & 1;
            int dn = (w >> 9) & 7;
            int src_an = w & 7;
            u16 ext = mac_read16(cpu->mem, op_pc[i] + 2);
            i32 d16 = (i16)ext;

            emit_advance_flush(&e);
            emit_read_g(&e, &rc, G_A(src_an), 8);
            if (d16 >= -128 && d16 <= 127) {
                xt_addi(&e, 8, 8, d16);
            } else {
                emit_load_imm(&e, 11, 12, (u32)d16);
                xt_add (&e, 8, 8, 11);
            }
            emit_l32r_at(&e, 9, lit_off[LITERAL_RAM_BOUNDS],
                         entry_off + e.len);
            xt_and  (&e, 10, 8, 9);
            emit_l32r_at(&e, 9, lit_off[LITERAL_ROM_BOUNDS],
                         entry_off + e.len);
            xt_and  (&e, 11, 8, 9);
            emit_l32r_at(&e, 9, lit_off[LITERAL_ROM_BASE],
                         entry_off + e.len);
            xt_xor  (&e, 11, 11, 9);
            xt_and  (&e, 12, 10, 11);
            emit_cache_flush(&e, &rc);
            i32 op_pc_mulm = 4, op_cyc_mulm = 74;
            xt_beqz (&e, 12, (i32)(6u + helper_step_after_flush_undo_size(&rc, op_pc_mulm, op_cyc_mulm)));
            emit_helper_step_after_flush_undo(&e, lit_off[HELPER_M68K_STEP],
                                              entry_off, &rc, op_pc_mulm, op_cyc_mulm);
            u32 jmulm_pos = e.len;
            xt_j    (&e, 4);
            /* Fast path: pick base via a10. */
            emit_l32r_at(&e, 9, lit_off[ADDR_RAM_BASE],
                         entry_off + e.len);
            xt_beqz (&e, 10, 6);
            emit_l32r_at(&e, 9, lit_off[ADDR_ROM_HOST_BASE],
                         entry_off + e.len);
            xt_add  (&e, 9, 9, 8);
            /* Read mem.W → a11. */
            xt_l8ui (&e, 11, 9, 0);
            xt_l8ui (&e, 12, 9, 1);
            xt_slli (&e, 11, 11, 8);
            xt_or   (&e, 11, 11, 12);
            /* Extend mem.W → a10 (sext for MULS, zext for MULU). */
            if (sgn) {
                xt_slli (&e, 10, 11, 16);
                xt_srai (&e, 10, 10, 16);
            } else {
                xt_extui(&e, 10, 11, 0, 15);
            }
            /* Read Dn, extend low .W → a8 (matching sign/unsigned). */
            emit_read_g(&e, &rc, G_D(dn), 8);
            if (sgn) {
                xt_slli (&e, 8, 8, 16);
                xt_srai (&e, 8, 8, 16);
            } else {
                xt_extui(&e, 8, 8, 0, 15);
            }
            /* MULL → a11 = .L product. */
            xt_mull (&e, 11, 8, 10);
            emit_write_g(&e, &rc, G_D(dn), 11);
            if (!flags_dead[i]) emit_logic_flags(&e, 11);
            emit_advance(&e, op_pc_mulm, op_cyc_mulm);

            u32 here_mulm = e.len;
            i32 jo_mulm = (i32)(here_mulm - jmulm_pos) - 4;
            u32 jw_mulm = ((u32)((u32)jo_mulm & 0x3FFFFu) << 6) | 0x06u;
            base[entry_off + jmulm_pos    ] = (u8)jw_mulm;
            base[entry_off + jmulm_pos + 1] = (u8)(jw_mulm >> 8);
            base[entry_off + jmulm_pos + 2] = (u8)(jw_mulm >> 16);

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
        } else if ((top == 0xC || top == 0x8) && szf == 2 && !((w >> 8) & 1)
                   && mode == 7 && (w & 7) == 0) {
            /* M6.111 — AND.L (xxx).W,Dn  /  OR.L (xxx).W,Dn — bench-hot
             * 0xC4B8 (AND.L (xxx).W,D2) at 21 K helpers / 100 M cyc on
             * bench's post-cycle-11898 path.
             *
             * Compile-time RAM check (M6.77/M6.108 pattern): if the
             * .W absolute addr is in RAM and 2-aligned, inline the
             * 4-byte BE read + AND/OR with Dn + write back + logic
             * flags. Else fall through to helper. Length 4, cycles 8. */
            int dn = (w >> 9) & 7;
            bool is_or = (top == 0x8);
            u16 ext = mac_read16(cpu->mem, op_pc[i] + 2);
            u32 abs_addr = (u32)(i32)(i16)ext;
            abs_addr &= 0xFFFFFFu;

            u32 ram_size = cpu->mem ? cpu->mem->ram_size : 0;
            bool overlay = cpu->mem ? cpu->mem->overlay : true;
            bool ram_pow2 = ram_size > 0 && (ram_size & (ram_size - 1)) == 0;
            bool addr_in_ram = !overlay && ram_pow2
                               && abs_addr < ram_size
                               && (abs_addr & 1) == 0;

            if (addr_in_ram) {
                emit_advance_flush(&e);
                emit_l32r_at(&e, 9, lit_off[ADDR_RAM_BASE],
                             entry_off + e.len);
                if ((i32)abs_addr >= -128 && (i32)abs_addr <= 127) {
                    xt_addi(&e, 9, 9, (i32)abs_addr);
                } else if ((i32)abs_addr >= -2048 && (i32)abs_addr <= 2047) {
                    xt_movi(&e, 10, (i32)abs_addr);
                    xt_add (&e, 9, 9, 10);
                } else {
                    emit_load_imm(&e, 10, 11, abs_addr);
                    xt_add (&e, 9, 9, 10);
                }
                /* Read 4 BE bytes into a10 (.L source value). */
                xt_l8ui (&e, 11, 9, 0);
                xt_l8ui (&e, 12, 9, 1);
                xt_slli (&e, 10, 11, 24);
                xt_slli (&e, 12, 12, 16);
                xt_or   (&e, 10, 10, 12);
                xt_l8ui (&e, 11, 9, 2);
                xt_l8ui (&e, 12, 9, 3);
                xt_slli (&e, 11, 11, 8);
                xt_or   (&e, 10, 10, 11);
                xt_or   (&e, 10, 10, 12);          /* a10 = .L value */

                /* Read Dn (cache-slot direct if cached). */
                int dn_xt = cache_lookup(&rc, G_D(dn));
                u8 dn_reg = (dn_xt >= 0) ? (u8)dn_xt : 11;
                if (dn_xt < 0) emit_read_g(&e, &rc, G_D(dn), 11);
                if (is_or) xt_or (&e, dn_reg, dn_reg, 10);
                else       xt_and(&e, dn_reg, dn_reg, 10);
                if (dn_xt >= 0) {
                    for (int s = 0; s < rc.active; s++)
                        if (rc.guest[s] == (u8)G_D(dn)) { rc.dirty |= (u16)(1u << s); break; }
                } else {
                    emit_write_g(&e, &rc, G_D(dn), 11);
                }
                if (!flags_dead[i]) emit_logic_flags(&e, dn_reg);
                emit_advance(&e, 4, 8);
                inline_ops++; done = true;
            }
            /* else: fall through to helper. */
        } else if (top == 0xB && szf == 2 && ((w >> 8) & 1) && mode == 0) {
            /* EOR.L Dn,Dm  (result -> Dm) */
            emit_logic_l_dd(&e, (w >> 9) & 7, w & 7, w & 7, true, &rc);
            inline_ops++; done = true;
        } else if (top == 0xC && (szf == 0 || szf == 1) && !((w >> 8) & 1) && mode == 0) {
            /* M6.100 — AND.B / AND.W Dm,Dn (dst = Dn at bits 11-9).
             * Boot-warm 0xC242 (AND.W D2,D1) at 525 helpers / 100 M cyc. */
            int size_bits = (szf == 0) ? 8 : 16;
            emit_logic_bw_dd_kind(&e, w & 7, (w >> 9) & 7, (w >> 9) & 7,
                                  1, size_bits, flags_dead[i], &rc);
            inline_ops++; done = true;
        } else if (top == 0x8 && (szf == 0 || szf == 1) && !((w >> 8) & 1) && mode == 0) {
            /* M6.100 — OR.B / OR.W Dm,Dn (dst = Dn at bits 11-9). */
            int size_bits = (szf == 0) ? 8 : 16;
            emit_logic_bw_dd_kind(&e, w & 7, (w >> 9) & 7, (w >> 9) & 7,
                                  0, size_bits, flags_dead[i], &rc);
            inline_ops++; done = true;
        } else if (top == 0xB && (szf == 0 || szf == 1) && ((w >> 8) & 1) && mode == 0) {
            /* M6.100 — EOR.B / EOR.W Dn,Dm (src = Dn at bits 11-9, dst =
             * Dm at bits 5-3/2-0). EOR only has the EA-dst form; for
             * mode=0 the EA is Dm. */
            int size_bits = (szf == 0) ? 8 : 16;
            emit_logic_bw_dd_kind(&e, (w >> 9) & 7, w & 7, w & 7,
                                  2, size_bits, flags_dead[i], &rc);
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
        } else if (top == 0x0 && !((w >> 8) & 1) && ((w >> 9) & 7) == 6
                   && szf == 1 && mode == 5) {
            /* CMPI.W #imm16, (d16,An) — bench-warm 0x0C6D (CMPI.W #imm,
             * (d16,A5)) at 515 hits/20 M cyc (M6.80).
             *
             * Read .W from An+sext_d16, sign-extend to high 16 of a32-bit
             * reg, subtract from sign-extended imm (also at high 16),
             * derive CMP flags from the .L-style subtract result (the
             * "shift to high 16" trick — same as CMP.W (d16,An),Dn at
             * line ~3023 — keeps the 32-bit emit_addsub_flags_long output
             * size-correct for the .W case).
             *
             * Length 6 (op + imm + d16), cycles 8. Source can be ROM, so
             * use the unified RAM/ROM bounds. CMP doesn't write, doesn't
             * touch X (so keep_x=true). */
            int src_an = w & 7;
            u16 imm     = mac_read16(cpu->mem, op_pc[i] + 2);
            u16 ext_d16 = mac_read16(cpu->mem, op_pc[i] + 4);
            i32 d16 = (i16)ext_d16;
            i32 imm32 = (i32)(i16)imm;

            emit_advance_flush(&e);
            emit_read_g(&e, &rc, G_A(src_an), 8);
            if (d16 >= -128 && d16 <= 127) {
                xt_addi(&e, 8, 8, d16);
            } else {
                emit_load_imm(&e, 11, 12, (u32)d16);
                xt_add (&e, 8, 8, 11);
            }
            emit_l32r_at(&e, 9, lit_off[LITERAL_RAM_BOUNDS],
                         entry_off + e.len);
            xt_and  (&e, 10, 8, 9);
            emit_l32r_at(&e, 9, lit_off[LITERAL_ROM_BOUNDS],
                         entry_off + e.len);
            xt_and  (&e, 11, 8, 9);
            emit_l32r_at(&e, 9, lit_off[LITERAL_ROM_BASE],
                         entry_off + e.len);
            xt_xor  (&e, 11, 11, 9);
            xt_and  (&e, 12, 10, 11);
            emit_cache_flush(&e, &rc);
            i32 op_pc_ci = 6, op_cyc_ci = 8;
            xt_beqz (&e, 12, (i32)(6u + helper_step_after_flush_undo_size(&rc, op_pc_ci, op_cyc_ci)));
            emit_helper_step_after_flush_undo(&e, lit_off[HELPER_M68K_STEP],
                                              entry_off, &rc, op_pc_ci, op_cyc_ci);
            u32 jci_pos = e.len;
            xt_j    (&e, 4);
            /* Fast path: pick base via a10. */
            emit_l32r_at(&e, 9, lit_off[ADDR_RAM_BASE],
                         entry_off + e.len);
            xt_beqz (&e, 10, 6);
            emit_l32r_at(&e, 9, lit_off[ADDR_ROM_HOST_BASE],
                         entry_off + e.len);
            xt_add  (&e, 9, 9, 8);
            /* Read .W (2 BE bytes) → a11. */
            xt_l8ui (&e, 11, 9, 0);
            xt_l8ui (&e, 12, 9, 1);
            xt_slli (&e, 11, 11, 8);
            xt_or   (&e, 11, 11, 12);
            /* Shift mem.W to high 16 (= d for the SUB) → a9. */
            xt_slli (&e, 9, 11, 16);
            /* imm.W shifted to high 16 → a8 (= s for the SUB). */
            emit_load_imm(&e, 8, 11, (u32)imm32);
            xt_slli (&e, 8, 8, 16);
            /* r = d - s → a10. */
            xt_sub  (&e, 10, 9, 8);
            if (!flags_dead[i]) {
                emit_addsub_flags_long_masked(&e, true, true, 8, 9, 10, flags_needed[i]);
            }
            emit_advance(&e, op_pc_ci, op_cyc_ci);

            u32 here_ci = e.len;
            i32 jo_ci = (i32)(here_ci - jci_pos) - 4;
            u32 jw_ci = ((u32)((u32)jo_ci & 0x3FFFFu) << 6) | 0x06u;
            base[entry_off + jci_pos    ] = (u8)jw_ci;
            base[entry_off + jci_pos + 1] = (u8)(jw_ci >> 8);
            base[entry_off + jci_pos + 2] = (u8)(jw_ci >> 16);

            inline_ops++; done = true;
        } else if (top == 0x0 && !((w >> 8) & 1) && ((w >> 9) & 7) == 4
                   && mode == 7 && (w & 7) == 0) {
            /* M6.113 — BTST / BCHG / BCLR / BSET #imm,(xxx).W. Static
             * bit op against a low-RAM absolute address (sign-extended
             * 16-bit ext word). Bench-hot 0x08F8 (BSET #imm,(xxx).W)
             * at 21 K helpers / 100 M cyc on the post-cycle-11898 path.
             *
             * `which = (w >> 6) & 3`: 0=BTST, 1=BCHG, 2=BCLR, 3=BSET.
             * For byte EA, bit number = imm_word & 7. Cycles 8 (m68k_step
             * base 4 + handler 4); length 6 (op + imm.W + abs.W).
             *
             * Sets ONLY Z = !old_bit. Other CCR bits unchanged.
             *
             * Compile-time RAM check on abs_addr (M6.77 pattern). For
             * non-BTST ops, the byte is read-modified-written; BTST just
             * reads and sets Z. */
            int which = szf;  /* (w >> 6) & 3 = 0..3 */
            u16 imm_word = mac_read16(cpu->mem, op_pc[i] + 2);
            int bit = imm_word & 7;
            u16 abs_word = mac_read16(cpu->mem, op_pc[i] + 4);
            u32 abs_addr = (u32)(i32)(i16)abs_word;
            abs_addr &= 0xFFFFFFu;

            u32 ram_size = cpu->mem ? cpu->mem->ram_size : 0;
            bool overlay = cpu->mem ? cpu->mem->overlay : true;
            bool ram_pow2 = ram_size > 0 && (ram_size & (ram_size - 1)) == 0;
            bool addr_in_ram = !overlay && ram_pow2 && abs_addr < ram_size;

            if (addr_in_ram) {
                emit_advance_flush(&e);
                emit_l32r_at(&e, 9, lit_off[ADDR_RAM_BASE],
                             entry_off + e.len);
                if ((i32)abs_addr >= -128 && (i32)abs_addr <= 127) {
                    xt_addi(&e, 9, 9, (i32)abs_addr);
                } else if ((i32)abs_addr >= -2048 && (i32)abs_addr <= 2047) {
                    xt_movi(&e, 10, (i32)abs_addr);
                    xt_add (&e, 9, 9, 10);
                } else {
                    emit_load_imm(&e, 10, 11, abs_addr);
                    xt_add (&e, 9, 9, 10);
                }
                xt_l8ui (&e, 10, 9, 0);                 /* a10 = byte */
                xt_extui(&e, 11, 10, (u8)bit, 0);       /* a11 = old bit (0/1) */

                /* For BCHG/BCLR/BSET: modify and write back. */
                if (which != 0) {
                    int mask = 1 << bit;
                    if (which == 1) {        /* BCHG: byte ^= mask */
                        xt_movi(&e, 12, mask);
                        xt_xor (&e, 10, 10, 12);
                    } else if (which == 2) { /* BCLR: byte &= ~mask */
                        xt_movi(&e, 12, ~mask & 0xFF);
                        xt_and (&e, 10, 10, 12);
                    } else {                 /* BSET: byte |= mask */
                        xt_movi(&e, 12, mask);
                        xt_or  (&e, 10, 10, 12);
                    }
                    xt_s8i(&e, 10, 9, 0);
                }

                /* Update SR.Z (bit 2): set if old bit was 0, clear else.
                 * Other CCR bits unchanged. */
                xt_movi(&e, 12, -5);                    /* ~0x04 */
                xt_and(&e, R_SR, R_SR, 12);
                xt_movi(&e, 12, 0x04);
                xt_bnez(&e, 11, 6);                     /* skip OR if bit set */
                xt_or(&e, R_SR, R_SR, 12);
                g_sr_dirty = true;
                sext_memo_invalidate();
                emit_advance(&e, 6, 8);
                inline_ops++; done = true;
            }
            /* else: fall through to helper. */
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
            g_block_emitted_callx0 = true;  /* M6.84 */
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
            g_block_emitted_callx0 = true;  /* M6.84 */
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
        } else if (top == 0xE && !((w >> 8) & 1)
                   && ((w >> 6) & 3) == 2
                   && !((w >> 5) & 1)
                   && (((w >> 3) & 3) == 1 || ((w >> 3) & 3) == 0)) {
            /* M6.99 — LSR.L / ASR.L #imm,Dn. Special-cased separately
             * from the .B/.W arm below because .L doesn't need the
             * size-bit extract or merge (the shift operates on the full
             * 32-bit Dn in-place). Length 2, cycles 4 + 8 + 2*imm. */
            int imm = (w >> 9) & 7; if (imm == 0) imm = 8;
            int dn = w & 7;
            bool is_asr = ((w >> 3) & 3) == 0;

            int dn_xt = cache_lookup(&rc, G_D(dn));
            u8 src_reg = (dn_xt >= 0) ? (u8)dn_xt : 8;
            if (dn_xt < 0) emit_read_g(&e, &rc, G_D(dn), 8);

            /* last_out MUST be captured before in-place shift writes
             * dst_reg (which is the same as src_reg when cached). */
            xt_extui(&e, 10, src_reg, imm - 1, 0);
            u8 dst_reg = (dn_xt >= 0) ? (u8)dn_xt : 8;
            if (is_asr) xt_srai(&e, dst_reg, src_reg, imm);
            else        xt_srli(&e, dst_reg, src_reg, imm);

            if (dn_xt >= 0) {
                for (int s = 0; s < rc.active; s++)
                    if (rc.guest[s] == (u8)G_D(dn)) { rc.dirty |= (u16)(1u << s); break; }
            } else {
                emit_write_g(&e, &rc, G_D(dn), 8);
            }

            if (!flags_dead[i]) {
                /* C=X=last_out (a10); Z=(result==0); N: 0 for LSR (zero
                 * fill), bit 31 of result for ASR. V=0. */
                xt_movi (&e, 12, -32);
                xt_and  (&e, R_SR, R_SR, 12);
                xt_or   (&e, R_SR, R_SR, 10);
                xt_slli (&e, 12, 10, 4);
                xt_or   (&e, R_SR, R_SR, 12);
                if (is_asr) {
                    xt_extui(&e, 12, dst_reg, 31, 0);
                    xt_slli (&e, 12, 12, 3);
                    xt_or   (&e, R_SR, R_SR, 12);
                }
                xt_movi (&e, 12, 0x04);
                xt_bnez (&e, dst_reg, 6);
                xt_or   (&e, R_SR, R_SR, 12);
                g_sr_dirty = true;
                sext_memo_invalidate();
            }
            emit_advance(&e, 2, 10 + 2 * imm);
            inline_ops++; done = true;
        } else if (top == 0xE && !((w >> 8) & 1)
                   && (((w >> 6) & 3) == 0 || ((w >> 6) & 3) == 1)
                   && !((w >> 5) & 1)
                   && (((w >> 3) & 3) == 1 || ((w >> 3) & 3) == 0)) {
            /* M6.97 / M6.98 — LSR.B/W / ASR.B/W #imm,Dn — boot's e442
             * (ASR.W #2,D2, 3 K) and e208 (ASR.B #1,D0, 780). The .B and
             * .W variants share value-merge structure parametrized on
             * size_bits and shift_count.
             *
             * Bit-field decode:
             *   bits 11-9 = count (1..7, 0 means 8)
             *   bit 8 = 0 (right shift)
             *   bits 7-6 = 00 (.B) or 01 (.W)
             *   bit 5 = 0 (immediate count)
             *   bits 4-3 = 00 (ASR) or 01 (LSR)
             *   bits 2-0 = Dn
             *
             * Semantics:
             *   LSR: result = Dn.size >> imm   (zero fill, N=0)
             *   ASR: result = sext(Dn.size) >> imm  (sign fill, N=sign)
             *   common: Z=(result==0); C=X=bit(imm-1) of Dn.size; V=0
             *           Dn = (Dn & ~size_mask) | (result & size_mask)
             *
             * Cycles: m68k_step base 4 + handler `6 + 2*cnt`. */
            int imm = (w >> 9) & 7; if (imm == 0) imm = 8;
            int dn = w & 7;
            int szf_local = (w >> 6) & 3;
            int size_bits = szf_local == 0 ? 8 : 16;     /* .B or .W */
            bool is_asr = ((w >> 3) & 3) == 0;

            int dn_xt = cache_lookup(&rc, G_D(dn));
            u8 src_reg = (dn_xt >= 0) ? (u8)dn_xt : 8;
            if (dn_xt < 0) emit_read_g(&e, &rc, G_D(dn), 8);

            /* a9 = Dn.size (zero-ext'd in low size_bits). For .B / .W
             * use xt_extui to pull the low bits. */
            xt_extui(&e, 9, src_reg, 0, size_bits - 1);
            xt_extui(&e, 10, 9, imm - 1, 0);             /* last_out */
            if (is_asr) {
                int up = 32 - size_bits;                  /* 24 for .B, 16 for .W */
                xt_slli (&e, 9, 9, up);                   /* sign at bit 31 */
                xt_srai (&e, 9, 9, up + imm);             /* arith shift */
                xt_extui(&e, 9, 9, 0, size_bits - 1);     /* mask to size bits */
            } else {
                xt_srli (&e, 9, 9, imm);
            }
            /* Merge: clear low size_bits of src_reg, then OR in result.
             * For .B: srli 8 then slli 8 (clears low 8). For .W: srli 16 then slli 16. */
            xt_srli (&e, 11, src_reg, size_bits);
            xt_slli (&e, 11, 11, size_bits);
            u8 dst_reg = (dn_xt >= 0) ? (u8)dn_xt : 8;
            xt_or   (&e, dst_reg, 11, 9);

            if (dn_xt >= 0) {
                for (int s = 0; s < rc.active; s++)
                    if (rc.guest[s] == (u8)G_D(dn)) { rc.dirty |= (u16)(1u << s); break; }
            } else {
                emit_write_g(&e, &rc, G_D(dn), 8);
            }

            if (!flags_dead[i]) {
                xt_movi (&e, 12, -32);                    /* mask = ~0x1F */
                xt_and  (&e, R_SR, R_SR, 12);
                xt_or   (&e, R_SR, R_SR, 10);             /* set C */
                xt_slli (&e, 12, 10, 4);
                xt_or   (&e, R_SR, R_SR, 12);             /* set X */
                if (is_asr) {
                    /* N = bit (size_bits-1) of result. */
                    xt_extui(&e, 12, 9, size_bits - 1, 0);
                    xt_slli (&e, 12, 12, 3);
                    xt_or   (&e, R_SR, R_SR, 12);
                }
                xt_movi (&e, 12, 0x04);
                xt_bnez (&e, 9, 6);
                xt_or   (&e, R_SR, R_SR, 12);
                g_sr_dirty = true;
                sext_memo_invalidate();
            }
            emit_advance(&e, 2, 10 + 2 * imm);
            inline_ops++; done = true;
        } else if (w == 0x46FC) {
            /* M6.117 — MOVE #imm16,SR. Privileged op. Bench-hot 21 598 hits
             * / 100 M cyc — the last remaining 21K-hit un-inlined opcode
             * on the post-cycle-11898 path.
             *
             * Length 4 (op + imm.W). Cycles 4 (m68k_step base 4, handler 0
             * for the MOVE-to-SR path in m68k_interp.c line 951-958).
             *
             * Correctness gates:
             *   1. Compile-time: only inline when imm has S bit (0x2000)
             *      set. If imm.S=0, we'd need a S→U transition with SP
             *      swap; defer to helper. Boot/bench's hot 0x46fc all
             *      have imm.S=1 (stays-in-supervisor pattern: disable
             *      interrupts via SR.I write while in kernel).
             *   2. Runtime: check current SR.S=1. If user mode, helper
             *      must run to take the privilege-violation trap. Since
             *      our compile-time imm.S=1, supervisor→supervisor has
             *      no SP swap — just write SR = imm.
             *
             * Fast path: SR = imm (1-3 ops). Plus the privilege check
             * (~3 ops). Helper bridge for the slow path (when user mode).
             *
             * Saves ~59 LX7 per fast-path hit vs 64-LX7 helper. */
            u16 imm = mac_read16(cpu->mem, op_pc[i] + 2);
            if (imm & 0x2000) {       /* imm.S = 1 → stay supervisor */
                emit_advance_flush(&e);
                emit_cache_flush(&e, &rc);

                /* a8 = current SR.S bit (0 or 1). msksize=0 ⇒ width=1
                 * (extracts bit 13 only). The earlier version used
                 * msksize=1 (width=2) which still functioned because
                 * bit 14 is reserved/0 on 68000, but msksize=0 is the
                 * precise extraction. */
                xt_extui(&e, 8, R_SR, 13, 0);

                /* xt_bnez (not beqz) — JUMP to fast path when S=1
                 * (supervisor, no trap needed). The earlier beqz had
                 * the condition inverted: it jumped to fast path when
                 * S=0 (user) — which would have written SR bypassing
                 * the privilege trap. Boot/bench is always S=1 so the
                 * beqz fell through to the helper bridge every time,
                 * making the fast path effectively unreachable. */
                i32 op_pc_sr = 4, op_cyc_sr = 4;
                u32 sr_bridge_size = helper_step_after_flush_undo_size(&rc, op_pc_sr, op_cyc_sr);
                xt_bnez(&e, 8, (i32)(6u + sr_bridge_size));

                /* Slow path: user mode → privilege-violation trap. */
                emit_helper_step_after_flush_undo(&e, lit_off[HELPER_M68K_STEP],
                                                  entry_off, &rc, op_pc_sr, op_cyc_sr);
                u32 jsr_pos = e.len;
                xt_j(&e, 4);

                /* Fast path: R_SR = imm. Use emit_load_imm because imm
                 * may exceed xt_movi's -2048..2047 range (e.g. imm=0x2700
                 * which is a typical "supervisor, disable interrupts"
                 * value). */
                emit_load_imm(&e, R_SR, 9, (u32)imm);
                g_sr_dirty = true;
                sext_memo_invalidate();
                emit_advance(&e, op_pc_sr, op_cyc_sr);

                u32 here_sr = e.len;
                i32 jo_sr = (i32)(here_sr - jsr_pos) - 4;
                u32 jw_sr = ((u32)((u32)jo_sr & 0x3FFFFu) << 6) | 0x06u;
                base[entry_off + jsr_pos    ] = (u8)jw_sr;
                base[entry_off + jsr_pos + 1] = (u8)(jw_sr >> 8);
                base[entry_off + jsr_pos + 2] = (u8)(jw_sr >> 16);

                inline_ops++; done = true;
            }
            /* else (imm.S=0): fall through to helper. */
        }

        if (!done) {
            /* Helper fallback: m68k_step(cpu). a3 survives the call. */
            emit_helper_step(&e, lit_off[HELPER_M68K_STEP], entry_off, &rc);
            helper_ops++;
        }
    }

    /* 6. Epilogue: flush accumulated PC/cycles deltas, dirty cache slots
     * and R_SR back to cpu_state; restore the CALL0 return PC; JX back
     * to the dispatcher.
     *
     * On ESP32, before the return, try native block chaining: if the
     * dispatcher's predicted_next matches cpu->pc and our chain budget
     * isn't exhausted, JX directly to the next block's entry without
     * round-tripping the dispatcher. Saves ~50 host cycles per chain
     * hit (~83 % of block transitions in bench, ~97 % in boot per the
     * existing chain-predictor stats).
     *
     * Host build (xt_sim) does NOT emit the chain check — sim runs one
     * block per invocation and a JX out of the current block's code
     * buffer would crash the sim. The cpu->current_block field is still
     * set by dispatcher for parity, just unused on host. */
    emit_advance_flush(&e);
    emit_cache_flush(&e, &rc);
    emit_sr_flush(&e);
#if defined(ESP_PLATFORM)
    /* Native chain epilogue. Layout (~11 ops):
     *   l32i a8, OFF_PC
     *   l32i a9, OFF_CURRENT_BLOCK         (a9 = m68k_block*)
     *   l32i a10, [a9 + predicted_next_pc] (a10 = predicted_next_pc)
     *   xor a11, a8, a10                    (zero iff match)
     *   bnez a11, FALLBACK
     *   l32i a10, [a9 + predicted_next]     (a10 = m68k_block* next, or NULL)
     *   beqz a10, FALLBACK
     *   l32i a11, OFF_CHAIN_BUDGET
     *   beqz a11, FALLBACK
     *   addi a11, a11, -1
     *   s32i a11, OFF_CHAIN_BUDGET
     *   s32i a10, OFF_CURRENT_BLOCK         (next block is now current)
     *   l32i a11, [a10 + code]              (a11 = next->code base)
     *   l32i a12, [a10 + entry_off]
     *   add a11, a11, a12
     *   jx a11
     * FALLBACK:
     *   l32i a0, OFF_JITRETPC
     *   jx a0
     */
    {
        u32 off_pred_pc    = (u32)offsetof(m68k_block, predicted_next_pc);
        u32 off_pred       = (u32)offsetof(m68k_block, predicted_next);
        u32 off_pred_entry = (u32)offsetof(m68k_block, predicted_next_entry);
        u32 off_cur        = (u32)offsetof(m68k_cpu, current_block);
        u32 off_budget     = (u32)offsetof(m68k_cpu, chain_budget);

        xt_l32i(&e, 8,  R_CPU, OFF_PC);
        xt_l32i(&e, 9,  R_CPU, off_cur);
        xt_l32i(&e, 10, 9,     off_pred_pc);
        /* a11 = 0 iff pc == predicted_next_pc */
        xt_xor (&e, 11, 8, 10);
        u32 bnez1_pos = e.len;
        xt_bnez(&e, 11, 0);                  /* placeholder — backpatch to FALLBACK */
        xt_l32i(&e, 10, 9, off_pred);        /* a10 = predicted_next ptr */
        u32 beqz1_pos = e.len;
        xt_beqz(&e, 10, 0);                  /* placeholder */
        xt_l32i(&e, 11, R_CPU, off_budget);
        u32 beqz2_pos = e.len;
        xt_beqz(&e, 11, 0);                  /* placeholder */
        xt_addi(&e, 11, 11, -1);
        xt_s32i(&e, 11, R_CPU, off_budget);
        xt_s32i(&e, 10, R_CPU, off_cur);     /* current_block = predicted_next */
        /* Restore a0 = jit_ret_pc BEFORE chaining: the next block's
         * prologue does `s32i a0, OFF_JITRETPC` to save its own
         * return-to-dispatcher address. M6.62 skip-prologue chain JX
         * also requires a0 correct, because the body_addr path doesn't
         * re-save (cpu->jit_ret_pc must already hold the dispatcher
         * return PC). */
        xt_l32i(&e, 0, R_CPU, OFF_JITRETPC);
        /* M6.62: load the predict-time-chosen entry from the *current*
         * block (not from predicted_next). Dispatcher picked either
         * next->entry_addr or next->body_addr based on cache+SR compat,
         * so the chain JX skips the entire prologue when compatible
         * (saves ~5 LX7 ops per chain hit on boot; 99.7 % match rate). */
        xt_l32i(&e, 11, 9, off_pred_entry);
        xt_jx  (&e, 11);
        u32 fallback_off = e.len;

        /* Back-patch the three early-exit branches to FALLBACK. */
        for (u32 p_idx = 0; p_idx < 3; p_idx++) {
            u32 pos = (p_idx == 0) ? bnez1_pos : (p_idx == 1) ? beqz1_pos : beqz2_pos;
            i32 rel = (i32)(fallback_off - pos);     /* fallback - br */
            i32 off = rel - 4;                       /* enc_bxxz adjustment */
            u32 imm12 = (u32)off & 0xFFFu;
            u8 sel_t = (p_idx == 0) ? 0x5 : 0x1;     /* bnez=5, beqz=1 */
            u8 as    = (p_idx == 0) ? 11 : (p_idx == 1) ? 10 : 11;
            u32 enc = ((u32)imm12 << 12) | ((u32)as << 8) | ((u32)sel_t << 4) | 0x6;
            base[entry_off + pos    ] = (u8)enc;
            base[entry_off + pos + 1] = (u8)(enc >> 8);
            base[entry_off + pos + 2] = (u8)(enc >> 16);
        }
    }
#endif
    /* M6.84 — only reload a0 from OFF_JITRETPC if some emit on the
     * compile-time path put a CALLX0 in the block (the only thing
     * that clobbers a0 between the prologue's `s32i a0` and here).
     * `helper_ops` only tracks the default helper bridge at the
     * bottom of the dispatch chain; conditional bridges inside
     * inline arms (MOVE.W (An), MOVEA.L (d16,An), etc.) are tracked
     * via `g_block_emitted_callx0` instead. Skipping the l32i saves
     * 1 LX7 op per helper-less-block invocation. */
    if (g_block_emitted_callx0) {
        xt_l32i(&e, 0, R_CPU, OFF_JITRETPC);
    }
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
    b->entry_addr = (void *)(base + entry_off);
    b->body_addr = (void *)(base + body_off);
    b->chain_entry_addr = (void *)(base + chain_entry_off);
    b->sr_loaded = (u8)(block_needs_sr_load ? 1 : 0);
    b->inline_ops = inline_ops;
    b->helper_ops = helper_ops;
    b->predicted_next = NULL;
    b->predicted_next_pc = 0xFFFFFFFFu;
    b->predicted_next_entry = NULL;
    /* Pack the rc into a u32: low nibble = active count, then 4 nibbles
     * giving the guest reg (0..15) for slots 0..3 (slot is unused → 0xF). */
    {
        u32 sig = (u32)rc.active & 0xFu;
        for (int s = 0; s < 4; s++) {
            u32 g = (s < rc.active) ? (rc.guest[s] & 0xFu) : 0xFu;
            sig |= g << (4 + s * 4);
        }
        b->cache_sig = sig;
    }
    b->last_used_cycle = cpu->cycles;
    b->last_op = op_word[n_ops - 1];
    b->last_op_pc = op_pc[n_ops - 1];
    b->hash_next = NULL;
    return b;
}

/* M6.71 — static successor analysis. See codegen.h for the contract. */
int m68k_block_static_successors(const m68k_block *b, struct mac_mem *mem,
                                 u32 out[2]) {
    u16 op     = b->last_op;
    u32 op_pc  = b->last_op_pc;
    int top    = (op >> 12) & 0xF;

    /* BRA / Bcc / BSR (top nibble 6).  Encoding: 0110 cccc dddddddd.
     * disp byte 0 → .W form (disp16 in next word); 0xFF → .L (68020,
     * not produced by 68000 Mac code — treated as dynamic to be safe). */
    if (top == 0x6) {
        int cc = (op >> 8) & 0xF;
        i32 disp8 = (i8)(op & 0xFF);
        u32 taken, ft;
        if (disp8 == 0) {
            taken = op_pc + 2 + (u32)(i32)(i16)mac_read16(mem, op_pc + 2);
            ft    = op_pc + 4;
        } else if (disp8 == -1) {
            return 0;
        } else {
            taken = op_pc + 2 + (u32)disp8;
            ft    = op_pc + 2;
        }
        if (cc == 0 || cc == 1) {            /* BRA / BSR — one successor. */
            out[0] = taken;
            return 1;
        }
        /* Bcc — two successors (taken + fall-through). */
        out[0] = taken;
        out[1] = ft;
        return 2;
    }

    /* DBcc Dn, disp16  —  encoding 0101 cccc 11 001 ddd. disp16 follows. */
    if (top == 0x5 && ((op >> 6) & 3) == 3 && ((op >> 3) & 7) == 1) {
        out[0] = op_pc + 4;                  /* fall-through */
        out[1] = op_pc + 2 + (u32)(i32)(i16)mac_read16(mem, op_pc + 2);
        return 2;
    }

    /* JMP / JSR with statically-known effective address. */
    switch (op) {
        case 0x4EF9:                         /* JMP (xxx).L */
        case 0x4EB9:                         /* JSR (xxx).L */
            out[0] = mac_read32(mem, op_pc + 2);
            return 1;
        case 0x4EF8:                         /* JMP (xxx).W */
        case 0x4EB8:                         /* JSR (xxx).W */
            out[0] = (u32)(i32)(i16)mac_read16(mem, op_pc + 2);
            return 1;
        case 0x4EFA:                         /* JMP (d16,PC) */
        case 0x4EBA:                         /* JSR (d16,PC) */
            out[0] = op_pc + 2 + (u32)(i32)(i16)mac_read16(mem, op_pc + 2);
            return 1;
        default: break;
    }

    /* Plain block-size-cap fall-through: the JIT block walker stopped
     * at M68K_MAX_OPS_PER_BLOCK without hitting a terminator. The
     * successor PC is just pc_end. Detect by: not a known dynamic
     * terminator. Dynamic = top-4 RTS/RTE/RTR/STOP/TRAP/JMP/JSR via An
     * or PC-relative-indexed, plus line-A/line-F traps. */
    if (top == 0xA || top == 0xF) return 0;
    if (top == 0x4) {
        if (op == 0x4E75 || op == 0x4E73 || op == 0x4E77 || op == 0x4E72)
            return 0;                        /* RTS / RTE / RTR / STOP */
        if ((op & 0xFFF0) == 0x4E40) return 0;       /* TRAP #n */
        if ((op & 0xFFC0) == 0x4EC0 || (op & 0xFFC0) == 0x4E80) {
            /* JMP / JSR <ea>. Only the (xxx).W/.L/(d16,PC) variants are
             * caught above; the rest (An, (An,Xn), (d8,PC,Xn)) are
             * dynamic. */
            return 0;
        }
    }
    out[0] = b->pc_end;
    return 1;
}

void m68k_block_free(m68k_block *b) {
    free(b);
}
