#ifndef XTENSA_SIM_H
#define XTENSA_SIM_H

#include "m68k_types.h"

/* Minimal Xtensa LX7 simulator. Covers ONLY the subset of instructions our
 * JIT emits (see emit_xtensa.c). Used on the host where the JIT code cannot
 * be executed natively (host is x86). On the ESP32-S3 target this sim is
 * not compiled — the CPU runs the code directly. */

/* Thunk signature: invoked when the sim executes CALLX0. `fn_token` is the
 * 32-bit value that was in the calling register (typically loaded via L32R
 * from the block's literal pool). `regs` is the full a0..a15 array so the
 * thunk can read arguments per CALL0 ABI (a2..a7) and write the return value
 * back into regs[2]. Returns nothing — mutations happen in-place. */
struct xt_sim;
typedef void (*xt_sim_call_thunk)(struct xt_sim *s, u32 fn_token);

typedef struct xt_sim {
    /* General-purpose registers a0..a15. */
    u32 a[16];

    /* Host-side trampoline for CALLX0. The sim looks up the function pointer
     * stored at the absolute sim-address in the target register (we expect
     * it to have been loaded via L32R from the literal pool of the block)
     * and calls back through this trampoline. If NULL, CALLX0 with a known
     * Xtensa function address is unsupported. */
    xt_sim_call_thunk call_thunk;
    /* Optional user data carried by the dispatcher. */
    void *user;

    /* The base of code being executed. PC is an offset within this block. */
    const u8 *code;
    u32 code_size;
    u32 pc;     /* byte offset within `code` */

    /* L32R literal pool resolver. PC-relative addresses pointing BEFORE code
     * are looked up via this callback; the sim returns the 32-bit literal at
     * the requested target address. */
    u32 (*read_literal)(struct xt_sim *s, u32 addr);

    /* Data memory access: routes loads/stores to host memory. The address
     * space here is host pointer-sized (we deal with 32-bit truncation by
     * having the JIT only generate accesses that fit). */
    u8 *(*translate)(struct xt_sim *s, u32 addr);

    /* Exit reason on stop. */
    enum {
        XT_SIM_RUN,
        XT_SIM_RETURNED,    /* RET hit (JX a0 with a0 = sentinel) */
        XT_SIM_BAD_OPCODE,  /* decoded an op we don't support */
        XT_SIM_OUT_OF_RANGE /* PC walked off the end */
    } status;
} xt_sim;

void xt_sim_init(xt_sim *s, const u8 *code, u32 code_size);

/* Run until status != XT_SIM_RUN. max_steps caps the runtime to catch infinite
 * loops in tests. */
void xt_sim_run(xt_sim *s, u32 max_steps);

#endif
