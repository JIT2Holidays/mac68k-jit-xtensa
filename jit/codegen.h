#ifndef CODEGEN_H
#define CODEGEN_H

#include "m68k_types.h"
#include "m68k_cpu.h"
#include "codecache.h"

/* JIT codegen: translate a 68000 basic block to native Xtensa LX7.
 *
 * The translation strategy is deliberately "basic" (see PLAN.md): a small
 * curated set of opcodes is emitted as inline native code; every other
 * instruction is a CALLX0 to the reference interpreter's m68k_step. The
 * fallback keeps the JIT trivially correct while the block batching and
 * the predicted-next chain give the speed-up over a pure interpreter.
 *
 * Block memory layout (one codecache allocation):
 *   +----------------------+ <- block->code
 *   |  literal pool        |   one u32 per literal_id
 *   +----------------------+ <- block->entry_off
 *   |  prologue            |   a3 = cpu_state base; stash return PC
 *   |  body                |   per-instruction: inline or CALLX0
 *   |  epilogue            |   reload return PC, JX
 *   +----------------------+
 *
 * Xtensa register convention inside a block (CALL0 ABI):
 *   a0  = return address (clobbered by CALLX0 — stashed in cpu->jit_ret_pc)
 *   a1  = stack pointer  (NEVER modified — see jit_trampolines.S)
 *   a2  = CALLX0 argument scratch
 *   a3  = cpu_state base address — survives helper calls (callee a2-a7 safe)
 *   a8..a15 = scratch for inline op bodies (clobbered by any helper call)
 */

/* Literal-pool entries. ADDR_* are base addresses for L32R + memory ops;
 * HELPER_* are CALLX0 targets. On the host build these are sentinels the
 * xtensa_sim's callbacks recognise; on the ESP32-S3 they are real pointers. */
typedef enum {
    ADDR_CPU_BASE = 0,
    HELPER_M68K_STEP,
    ADDR_RAM_BASE,        /* guest RAM base (sim's mapped address). */
    LITERAL_RAM_BOUNDS,   /* ~(ram_size-1) | 1 — fails the AND fast-path */
                          /* test for any out-of-RAM or odd address. */
    LITERAL_BCC_PC,       /* per-block: BRA.S taken or Bcc.S fall-through PC. */
                          /* Filled by codegen, not by the helper_addr cb. */
    HELPER_JIT_ORI_B_MMIO,/* fast-path ORI.B (d16,An) helper for MMIO targets */
    HELPER_JIT_BTST_B_MMIO,/* fast-path BTST (d16,An) helper for MMIO targets */
    HELPER_JIT_MOVEM_L_POSTINC,  /* MOVEM.L (An)+,reglist fast helper */
    HELPER_JIT_MOVEM_L_PREDEC,   /* MOVEM.L reglist,-(An) fast helper */
    HELPER_JIT_MOVEM_W_TO_MEM,   /* MOVEM.W reglist,(An) fast helper */
    HELPER_JIT_MOVEM_L_TO_MEM,   /* MOVEM.L reglist,(An) fast helper */
    HELPER_JIT_MOVE_W_POSTINC_TO_DN, /* MOVE.W (An)+,Dn fast helper for MMIO */
    HELPER_JIT_RTS_MMIO,             /* M6.132 — RTS fast helper for SP→MMIO */
    HELPER_JIT_BSR_S_MMIO,           /* M6.132 — BSR.S fast helper for SP→MMIO */
    HELPER_JIT_BSR_W_MMIO,           /* M6.132 — BSR.W fast helper for SP→MMIO */
    HELPER_JIT_MOVE_L_POSTINC_TO_DN_MMIO, /* M6.132 — MOVE.L (An)+,Dn fast helper for MMIO */
    HELPER_JIT_MOVE_B_ADDR_TO_DN_MMIO,    /* M6.133 — MOVE.B addr→Dn fast helper (shared) */
    HELPER_JIT_MOVE_B_DN_TO_ADDR_MMIO,    /* M6.133 — MOVE.B Dn→addr fast helper */
    HELPER_JIT_MOVE_L_DN_TO_ANPI_MMIO,    /* M6.133 — MOVE.L Dn|Am,(An)+ fast helper */
    HELPER_JIT_MOVE_B_ADDR_TO_AN_MMIO,    /* M6.134 — MOVE.B addr→(Am) fast helper */
    HELPER_JIT_MOVE_B_IMM_TO_ADDR_MMIO,   /* M6.135 — MOVE.B #imm,addr fast helper */
    HELPER_JIT_FLINE_TRAP,                /* M6.137 — F-line exception fast helper */
    HELPER_JIT_MOVE_L_AN_TO_DN_MMIO,      /* M6.144 — MOVE.L (An),Dn MMIO fast helper */
    HELPER_JIT_CLR_W_ANPI_MMIO,           /* M6.161 — CLR.W (An)+ MMIO fast helper */
    HELPER_JIT_TST_B_MMIO,                /* M6.169 — TST.B (d16,An) MMIO fast helper */
    HELPER_JIT_ALINE_TRAP,                /* M6.190 — A-line trap fast helper */
    HELPER_JIT_MOVE_ANPI_TO_SR,           /* M6.193 — MOVE (An)+,SR fast helper */
    HELPER_JIT_RTE,                       /* M6.198 — RTE fast helper */
    HELPER_JIT_BITOP_DN_AN_MMIO,          /* M6.204 — BTST/BCHG/BCLR/BSET Dn,(An) MMIO fast helper */
    HELPER_JIT_MOVE_L_XXXW_TO_AN_MMIO,    /* M6.225 — MOVE.L (xxx).W,(An) MMIO fast helper */
    HELPER_JIT_MOVE_B_ADDR_TO_ADDR_MMIO,  /* M6.228 — MOVE.B (d16,An),(d16,Am) mem-to-mem MMIO fast helper */
    HELPER_JIT_MOVE_L_ADDR_TO_ADDR_MMIO,  /* M6.229 — MOVE.L src,dst mem-to-mem MMIO fast helper */
    HELPER_JIT_MOVE_W_ADDR_TO_DN_MMIO,    /* M6.239 — MOVE.W addr,Dn MMIO fast helper */
    HELPER_JIT_MOVE_B_POSTINC_TO_DN_MMIO, /* M6.240 — MOVE.B (An)+,Dn MMIO fast helper */
    HELPER_JIT_MOVEA_L_ADDR_TO_AM_MMIO,   /* M6.240d — MOVEA.L addr,Am MMIO fast helper */
    HELPER_JIT_CMP_W_ADDR_DN_MMIO,        /* M6.241 — CMP.W (addr),Dn MMIO fast helper */
    HELPER_JIT_MOVE_W_ADDR_TO_POSTINC_MMIO, /* M6.242 — MOVE.W (addr),(Am)+ MMIO fast helper */
    /* M6.76 — ROM-source read fast path. Used by the MOVE.L (An)+,(Am)+
     * mem-to-mem inline arm so that bench's ROM-resident pointer-table
     * reads (~71K hits/20M cyc) can take a fast path instead of falling
     * to m68k_step. The pair is also a reusable building block for any
     * future "src can be RAM or ROM" inline. */
    LITERAL_ROM_BOUNDS,   /* ~(rom_size-1) | 1 — fails AND for non-ROM addrs */
    LITERAL_ROM_BASE,     /* MAC_ROM_BASE (0x400000) — xor-target for the */
                          /* in-ROM range check after the AND mask. */
    ADDR_ROM_HOST_BASE,   /* host ROM pointer shifted: rom_ptr - 0x400000, so */
                          /* base + addr lands in the host rom[] array for any */
                          /* admitted ROM-range guest address. */
    /* M6.91 — byte-aligned versions of the RAM/ROM bounds. The plain
     * LITERAL_RAM_BOUNDS has `| 1` to fail odd addresses (required for
     * .W/.L access); for MOVE.B-family ops, any byte address in RAM is
     * legal, so the byte variant drops the `| 1` to admit them. Used by
     * the MOVE.B inline arms that need RAM-only fast paths. */
    LITERAL_RAM_BOUNDS_BYTE,  /* ~(ram_size-1) — admits any byte addr in RAM */
    LITERAL_ROM_BOUNDS_BYTE,  /* ~(rom_size-1) — admits any byte addr in ROM */
    LITERAL_COUNT
} literal_id;

typedef u32 (*jit_helper_addr_fn)(literal_id id, void *user);

#define M68K_JIT_BLOCK_BUCKETS 2048u
#define M68K_MAX_OPS_PER_BLOCK 64u

typedef struct m68k_block {
    u32  pc_start;
    u32  pc_end;        /* fall-through PC (exclusive)        */
    u32  n_ops;
    u8  *code;          /* literal pool + code, 4-byte aligned */
    u32  code_size;
    u32  entry_off;     /* offset of the first prologue instruction */
    void *entry_addr;   /* precomputed `code + entry_off`. Native chain
                         * epilogue (ESP32) does one l32i + jx instead of
                         * l32i+l32i+add+jx — saves 2 LX7 ops per chain hit. */
    void *body_addr;    /* M6.62 cross-block-cache fast-chain entry: address
                         * of the first body instruction, *after* the
                         * prologue (cpu_base load, jit_ret_pc save,
                         * sr_reload, cache_load). Chain JX uses this when
                         * prev block's cache + SR state is compatible
                         * (see dispatcher's predict-time selection). */
    void *chain_entry_addr; /* M6.82 partial-prologue skip for chain-hit/no-
                         * cache-compat case. Points AFTER the prologue's
                         * `l32r R_CPU` and `s32i a0, OFF_JITRETPC` ops
                         * (both redundant on chain transitions — R_CPU
                         * = a3 is callee-saved across the JX, and the
                         * predecessor's chain epilogue already reloaded
                         * a0 from OFF_JITRETPC before the JX so the
                         * s32i would write the same value back). Saves
                         * 2 LX7 ops per chain-hit-no-cache-compat
                         * dispatch (which on bench is 96.7 % of chain
                         * transitions). */
    u8   sr_loaded;     /* M6.62: whether prologue did `l32i a14, OFF_SR`
                         * — i.e., a14 holds a valid SR at block start.
                         * Used by predict-time compat check: a chained
                         * successor that *needs* SR can only skip the
                         * prologue if the predecessor *also* loaded SR. */

    u32  inline_ops;    /* how many ops were translated inline       */
    u32  helper_ops;    /* how many fell back to the interpreter     */

    /* Single-slot next-block predictor — when this block falls through to
     * a fixed PC, the dispatcher caches the successor here. */
    struct m68k_block *predicted_next;
    u32                predicted_next_pc;
    void              *predicted_next_entry;   /* M6.62: precomputed JX target —
                                                * either next->entry_addr (full
                                                * prologue) or next->body_addr
                                                * (skip prologue, registers
                                                * already valid from this block). */

    /* Cache configuration signature: packs (active, guest[0..3]) so two
     * blocks with identical cache layouts compare equal. Used to gate the
     * cross-block-cache optimization: when prev->cache_sig == next->cache_sig
     * the chained successor can skip its prologue cache reload (the values
     * are still in a4..a7 from prev's execution). 0xFFFF marks "no sig". */
    u32                cache_sig;

    /* M6.63 LRU-eviction tag: cpu->cycles when this block was last
     * dispatched. The LRU evict callback walks every cached block to
     * find the smallest tag and evicts that block. Bump and FIFO modes
     * don't read this field. */
    u64                last_used_cycle;

    /* M6.71 prefetch — terminator opcode + the guest PC it's at. Lets
     * the dispatcher's prefetch policy analyse the block's static
     * successors (BRA / Bcc / JMP .L / DBcc / BSR / plain fall-through)
     * without re-walking the body. */
    u16                last_op;
    u32                last_op_pc;

    struct m68k_block *hash_next;   /* bucket chain */
} m68k_block;

/* Compile one basic block starting at `pc`. Returns NULL on failure (the
 * dispatcher then interp-falls-back for that PC). */
m68k_block *m68k_compile_block(codecache *cc, m68k_cpu *cpu, u32 pc,
                               jit_helper_addr_fn helper_addr, void *user);

/* M6.71 — fills `out[]` with the statically-known successor PCs of
 * block `b` and returns the count (0..2). Returns 0 for blocks ending
 * with a dynamic-target terminator (RTS / JMP (An) / JSR (An) / TRAP /
 * RTE / STOP / line-A / line-F) and 1-2 for static-target ones (BRA,
 * Bcc, JMP (xxx).L, BSR, DBcc, plain block-size-cap fall-through).
 * `mem` is needed to read multi-word displacements. */
struct mac_mem;
int m68k_block_static_successors(const m68k_block *b, struct mac_mem *mem,
                                 u32 out[2]);

void m68k_block_free(m68k_block *b);

#endif
