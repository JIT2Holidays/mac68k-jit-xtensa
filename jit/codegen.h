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

    u32  inline_ops;    /* how many ops were translated inline       */
    u32  helper_ops;    /* how many fell back to the interpreter     */

    /* Single-slot next-block predictor — when this block falls through to
     * a fixed PC, the dispatcher caches the successor here. */
    struct m68k_block *predicted_next;
    u32                predicted_next_pc;

    struct m68k_block *hash_next;   /* bucket chain */
} m68k_block;

/* Compile one basic block starting at `pc`. Returns NULL on failure (the
 * dispatcher then interp-falls-back for that PC). */
m68k_block *m68k_compile_block(codecache *cc, m68k_cpu *cpu, u32 pc,
                               jit_helper_addr_fn helper_addr, void *user);

void m68k_block_free(m68k_block *b);

#endif
