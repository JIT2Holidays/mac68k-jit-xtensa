#ifndef DISPATCHER_H
#define DISPATCHER_H

#include "m68k_types.h"
#include "m68k_cpu.h"
#include "codecache.h"
#include "codegen.h"

/* JIT dispatcher: owns the codecache arena and the block hash table, and
 * drives the find-or-compile / enter-block loop.
 *
 * On the ESP32-S3 a compiled block executes natively; on the host build
 * (where the CPU is x86) it runs through jit/xtensa_sim.c. Both paths go
 * through enter_block(), selected by #ifdef ESP_PLATFORM. */

#ifndef M68K_JIT_ARENA_KB
#define M68K_JIT_ARENA_KB 1024u
#endif

/* Estimated LX7-instruction cost of one CALLX0→m68k_step helper fallback
 * (the reference interpreter's average per-opcode cost). Used only to
 * weight helper calls in the host JIT-cost benchmark metric. */
#ifndef M68K_JIT_HELPER_LX7_COST
#define M68K_JIT_HELPER_LX7_COST 64u
#endif

typedef struct m68k_dispatcher {
    m68k_cpu  *cpu;
    codecache  cc;
    void      *arena;
    u32        arena_cap;

    m68k_block *buckets[M68K_JIT_BLOCK_BUCKETS];

    /* Stats. */
    u64 blocks_compiled;
    u64 blocks_executed;
    u64 helper_ops_total;
    u64 inline_ops_total;
    u64 chain_hits;
    u64 chain_misses;
    u64 interp_fallbacks;   /* ops run via m68k_step when compile failed */
    u64 arena_resets;
    u64 smc_invalidations;

    /* Host-only JIT-cost accounting (xtensa_sim). xt_instrs is the number
     * of native Xtensa instructions the generated code ran (≈ LX7 cycles
     * on the real target); helper_calls is the CALLX0→m68k_step count.
     * The estimated LX7 cost of the workload is
     *   xt_instrs + helper_calls * M68K_JIT_HELPER_LX7_COST.
     * This is the benchmark metric the optimisation loop minimises. */
    u64 xt_instrs;
    u64 helper_calls;

    bool no_cache;          /* bench toggle: recompile every dispatch */

    /* Self-modifying-code tracking. The guest OS loads code segments into
     * RAM and reuses that RAM, so a cached block can outlive the code it
     * was compiled from. `code_pages` marks 256-byte pages that hold
     * compiled code; a guest write to such a page queues it in
     * `dirty_pages`, and the run loop drops the affected blocks between
     * block executions (never while one is running). */
    u8   code_pages[8192];          /* bitmap: 65536 x 256-byte pages */
    u16  dirty_pages[256];
    int  n_dirty;
    bool smc_overflow;              /* dirty queue full -> full flush */
} m68k_dispatcher;

bool m68k_dispatcher_init(m68k_dispatcher *d, m68k_cpu *cpu);
void m68k_dispatcher_shutdown(m68k_dispatcher *d);

/* Run compiled blocks until cpu->cycles >= until or the CPU halts. */
void m68k_dispatcher_run_until(m68k_dispatcher *d, u64 until);

#endif
