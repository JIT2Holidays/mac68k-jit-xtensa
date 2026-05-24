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
    u64 chain_cache_matches;  /* of chain_hits, how many had prev->cache_sig == next->cache_sig */
    u64 prefetch_compiles;    /* M6.71 — blocks compiled via the static-successor prefetch path */
    u64 prefetch_hits;        /* of chain_misses, how many resolved to a block prefetch had already compiled */
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

    /* M6.71 / M6.72 — prefetch policy:
     *  PREFETCH_NONE   — no speculative compilation (default).
     *  PREFETCH_STATIC — after compile, also compile every statically-
     *                    known successor (Bcc-both-branches included),
     *                    depth 1. M6.71.
     *  PREFETCH_CHAIN  — only follow *unambiguous* successors (BRA /
     *                    BSR / JMP / fall-through, NOT Bcc / DBcc),
     *                    but follow them to depth `prefetch_depth`.
     *                    Reduces compile waste from cold conditional
     *                    branches and amortises more cold-start cost
     *                    on linear-chain code. M6.72. */
    u8   prefetch_mode;
    u8   prefetch_depth;    /* CHAIN-mode follow depth (default 2). */

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
/* Extended init: pick an arena size (KB) and an eviction policy
   (CC_MODE_BUMP / CC_MODE_LRU / CC_MODE_FIFO). The plain init uses
   M68K_JIT_ARENA_KB and CC_MODE_BUMP. */
bool m68k_dispatcher_init_ex(m68k_dispatcher *d, m68k_cpu *cpu,
                             u32 arena_kb, u8 evict_mode);
/* Prefetch policy values for m68k_dispatcher_set_prefetch. */
enum {
    PREFETCH_NONE   = 0,
    PREFETCH_STATIC = 1,
    PREFETCH_CHAIN  = 2,
};
/* Set the prefetch policy + chain-follow depth (used by CHAIN only).
 * Cheap to flip at runtime — only affects what get_block does
 * post-compile. Passing `depth = 0` is treated as the default (2). */
void m68k_dispatcher_set_prefetch(m68k_dispatcher *d, u8 mode, u8 depth);
void m68k_dispatcher_shutdown(m68k_dispatcher *d);

/* Run compiled blocks until cpu->cycles >= until or the CPU halts. */
void m68k_dispatcher_run_until(m68k_dispatcher *d, u64 until);

#endif
