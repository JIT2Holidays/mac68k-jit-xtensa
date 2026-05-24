/* JIT dispatcher. See dispatcher.h.
 *
 * The host build executes compiled blocks through the in-tree Xtensa
 * simulator (jit/xtensa_sim.c); the ESP32-S3 build runs them natively
 * via the CALL0<->windowed trampoline in port/esp32s3. */

#include "dispatcher.h"
#include "m68k_interp.h"
#include "mac_mem.h"
#include "sony.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !defined(ESP_PLATFORM)
#include "xtensa_sim.h"
#include <stdio.h>
/* Sentinel address ranges the host simulator's translate callback maps
 * back onto real host memory. Chosen well clear of any plausible small
 * sim-internal address. */
#define HOST_CPU_BASE    0x40000000u
#define HOST_STACK_BASE  0x50000000u
#define HOST_STACK_TOP   (HOST_STACK_BASE + 0x800u)
#define HOST_RAM_BASE    0x60000000u
#endif

/* --- literal-pool resolver -------------------------------------------- */

#if defined(ESP_PLATFORM)
/* CALL0 trampoline into the windowed reference interpreter. */
extern void m68k_step_call0(m68k_cpu *cpu);
#endif

/* RAM bounds mask: bit-AND a guest address against this; result is zero
 * only when the address is in RAM and 16-bit-aligned. For RAM sizes that
 * are a power of two (1 / 2 / 4 MB) the mask is `~(ram_size-1) | 1`. The
 * overlay bit forces the mask to all-ones, sending every fast-path probe
 * to the helper while ROM is mapped at 0. */
static u32 ram_bounds_mask(m68k_cpu *cpu) {
    if (!cpu || !cpu->mem) return 0xFFFFFFFFu;
    if (cpu->mem->overlay) return 0xFFFFFFFFu;
    u32 sz = cpu->mem->ram_size;
    if (sz == 0 || (sz & (sz - 1))) return 0xFFFFFFFFu;  /* non-pow2 → off */
    return (~(sz - 1u)) | 1u;
}

/* M6.76 — parallel ROM bounds mask. Failing the AND fast-path test means
 * "addr is NOT a power-of-two-aligned even ROM address in the canonical
 * [MAC_ROM_BASE, MAC_ROM_BASE + rom_size) range." During overlay the whole
 * map shifts, so we disable the ROM fast path like ram_bounds_mask does. */
static u32 rom_bounds_mask(m68k_cpu *cpu) {
    if (!cpu || !cpu->mem) return 0xFFFFFFFFu;
    if (cpu->mem->overlay) return 0xFFFFFFFFu;
    u32 sz = cpu->mem->rom_size;
    if (sz == 0 || (sz & (sz - 1))) return 0xFFFFFFFFu;  /* non-pow2 → off */
    return (~(sz - 1u)) | 1u;
}

static u32 helper_addr(literal_id id, void *user) {
    m68k_cpu *cpu = (m68k_cpu *)user;
#if defined(ESP_PLATFORM)
    switch (id) {
        case ADDR_CPU_BASE:     return (u32)(uintptr_t)cpu;
        case HELPER_M68K_STEP:  return (u32)(uintptr_t)&m68k_step_call0;
        case ADDR_RAM_BASE:     return (u32)(uintptr_t)cpu->mem->ram;
        case LITERAL_RAM_BOUNDS:return ram_bounds_mask(cpu);
        case HELPER_JIT_ORI_B_MMIO: return (u32)(uintptr_t)&m68k_jit_ori_b_mmio;
        case HELPER_JIT_BTST_B_MMIO: return (u32)(uintptr_t)&m68k_jit_btst_b_mmio;
        case HELPER_JIT_MOVEM_L_POSTINC: return (u32)(uintptr_t)&m68k_jit_movem_l_postinc_to_regs;
        case HELPER_JIT_MOVEM_L_PREDEC:  return (u32)(uintptr_t)&m68k_jit_movem_l_predec_from_regs;
        case HELPER_JIT_MOVEM_W_TO_MEM:  return (u32)(uintptr_t)&m68k_jit_movem_w_to_mem;
        case HELPER_JIT_MOVEM_L_TO_MEM:  return (u32)(uintptr_t)&m68k_jit_movem_l_to_mem;
        case HELPER_JIT_MOVE_W_POSTINC_TO_DN: return (u32)(uintptr_t)&m68k_jit_move_w_postinc_to_dn;
        case LITERAL_ROM_BOUNDS:return rom_bounds_mask(cpu);
        case LITERAL_ROM_BASE:  return (cpu && cpu->mem && cpu->mem->rom) ? MAC_ROM_BASE : 0xFFFFFFFFu;
        /* host_ptr - guest_base, so `host_ptr + guest_addr` lands at
         * the host's rom[] entry for the matching guest ROM address. */
        case ADDR_ROM_HOST_BASE:
            return (cpu && cpu->mem && cpu->mem->rom)
                ? ((u32)(uintptr_t)cpu->mem->rom - MAC_ROM_BASE)
                : 0u;
        default:                return 0;
    }
#else
    /* Host: the literal values are sentinels the sim's callbacks decode. */
    switch (id) {
        case ADDR_CPU_BASE:     return HOST_CPU_BASE;
        case HELPER_M68K_STEP:  return (u32)HELPER_M68K_STEP;
        case ADDR_RAM_BASE:     return HOST_RAM_BASE;
        case LITERAL_RAM_BOUNDS:return ram_bounds_mask(cpu);
        case HELPER_JIT_ORI_B_MMIO: return (u32)HELPER_JIT_ORI_B_MMIO;
        case HELPER_JIT_BTST_B_MMIO: return (u32)HELPER_JIT_BTST_B_MMIO;
        case HELPER_JIT_MOVEM_L_POSTINC: return (u32)HELPER_JIT_MOVEM_L_POSTINC;
        case HELPER_JIT_MOVEM_L_PREDEC:  return (u32)HELPER_JIT_MOVEM_L_PREDEC;
        case HELPER_JIT_MOVEM_W_TO_MEM:  return (u32)HELPER_JIT_MOVEM_W_TO_MEM;
        case HELPER_JIT_MOVEM_L_TO_MEM:  return (u32)HELPER_JIT_MOVEM_L_TO_MEM;
        case HELPER_JIT_MOVE_W_POSTINC_TO_DN: return (u32)HELPER_JIT_MOVE_W_POSTINC_TO_DN;
        case LITERAL_ROM_BOUNDS:return rom_bounds_mask(cpu);
        case LITERAL_ROM_BASE:  return (cpu && cpu->mem && cpu->mem->rom) ? MAC_ROM_BASE : 0xFFFFFFFFu;
        /* The host sim's translate maps HOST_RAM_BASE + (0x400000..rom_top)
         * to mem->rom for the matching guest range, so we can re-use the
         * same sentinel base on host — the sim's auto-route handles ROM. */
        case ADDR_ROM_HOST_BASE:
            return (cpu && cpu->mem && cpu->mem->rom) ? HOST_RAM_BASE : 0u;
        default:                return 0;
    }
#endif
}

/* --- block hash table ------------------------------------------------- */

static u32 bucket_of(u32 pc) {
    return (pc >> 1) & (M68K_JIT_BLOCK_BUCKETS - 1u);
}

static m68k_block *find_block(m68k_dispatcher *d, u32 pc) {
    for (m68k_block *b = d->buckets[bucket_of(pc)]; b; b = b->hash_next) {
        if (b->pc_start == pc) return b;
    }
    return NULL;
}

static void insert_block(m68k_dispatcher *d, m68k_block *b) {
    u32 bk = bucket_of(b->pc_start);
    b->hash_next = d->buckets[bk];
    d->buckets[bk] = b;
}

static void free_all_blocks(m68k_dispatcher *d) {
    for (u32 i = 0; i < M68K_JIT_BLOCK_BUCKETS; i++) {
        m68k_block *b = d->buckets[i];
        while (b) {
            m68k_block *next = b->hash_next;
            m68k_block_free(b);
            b = next;
        }
        d->buckets[i] = NULL;
    }
}

/* --- self-modifying-code tracking ------------------------------------- */

/* A guest write that lands on a page holding compiled code queues that
 * page for invalidation. Registered as the mac_mem write-watch. */
static void smc_watch(void *ctx, u32 addr) {
    m68k_dispatcher *d = (m68k_dispatcher *)ctx;
    u32 page = (addr >> 8) & 0xFFFFu;
    if (!(d->code_pages[page >> 3] & (1u << (page & 7)))) return;
    /* Native chaining (ESP32, M6.54) lets a block JX directly to its
     * predicted successor without returning to the dispatcher. That
     * bypasses smc_flush — so a chained block writing to a code page
     * could chain into a stale block before the dispatcher gets a turn.
     * Zeroing chain_budget here forces the next block's chain check
     * (the `beqz a11, FALLBACK` on the budget) to fall back, returning
     * to the dispatcher where smc_flush will drop the affected blocks.
     * No-op on host (chain epilogue not emitted there). */
    d->cpu->chain_budget = 0;
    for (int i = 0; i < d->n_dirty; i++)
        if (d->dirty_pages[i] == page) return;
    if (d->n_dirty < (int)(sizeof(d->dirty_pages) / sizeof(d->dirty_pages[0])))
        d->dirty_pages[d->n_dirty++] = (u16)page;
    else
        d->smc_overflow = true;
}

/* Mark every 256-byte page a freshly compiled block occupies. */
static void smc_mark_block(m68k_dispatcher *d, m68k_block *b) {
    for (u32 a = b->pc_start; a < b->pc_end; a += 256) {
        u32 page = (a >> 8) & 0xFFFFu;
        d->code_pages[page >> 3] |= (u8)(1u << (page & 7));
    }
    u32 last = (((b->pc_end ? b->pc_end - 1 : 0) >> 8)) & 0xFFFFu;
    d->code_pages[last >> 3] |= (u8)(1u << (last & 7));
}

/* Drop every cached block overlapping [lo, hi). */
static void smc_drop_range(m68k_dispatcher *d, u32 lo, u32 hi) {
    for (u32 i = 0; i < M68K_JIT_BLOCK_BUCKETS; i++) {
        m68k_block **pp = &d->buckets[i];
        while (*pp) {
            m68k_block *b = *pp;
            if (b->pc_start < hi && b->pc_end > lo) {
                *pp = b->hash_next;
                m68k_block_free(b);
                d->smc_invalidations++;
            } else {
                pp = &b->hash_next;
            }
        }
    }
}

/* Process queued dirty pages. Returns true if anything was invalidated
 * (so the caller drops its cached `prev` block pointer). */
static bool smc_flush(m68k_dispatcher *d) {
    if (!d->smc_overflow && d->n_dirty == 0) return false;

    if (d->smc_overflow) {
        free_all_blocks(d);
        codecache_reset(&d->cc);
        memset(d->code_pages, 0, sizeof(d->code_pages));
        d->arena_resets++;
    } else {
        for (int i = 0; i < d->n_dirty; i++) {
            u32 base = (u32)d->dirty_pages[i] << 8;
            smc_drop_range(d, base, base + 256u);
        }
    }
    d->n_dirty = 0;
    d->smc_overflow = false;

    /* Predicted-next pointers may now dangle — clear them all. */
    for (u32 i = 0; i < M68K_JIT_BLOCK_BUCKETS; i++)
        for (m68k_block *b = d->buckets[i]; b; b = b->hash_next) {
            b->predicted_next = NULL;
            b->predicted_next_pc = 0xFFFFFFFFu;
            b->predicted_next_entry = NULL;
        }
    return true;
}

/* --- init / shutdown -------------------------------------------------- */

/* LRU-mode eviction: walk all cached blocks, pick the one with the
 * lowest last_used_cycle, drop it from the hash + free its codecache
 * span. Returns the bytes returned to the free list, or 0 if nothing
 * could be evicted (empty cache).
 *
 * Cost: O(N_blocks). On boot's ~100 K blocks this is ~100 K iterations
 * per eviction — significant; mitigated by the fact that evictions
 * only happen when the arena fills, and only one per failed alloc. */
static u32 lru_evict_cb(void *ctx) {
    m68k_dispatcher *d = (m68k_dispatcher *)ctx;
    m68k_block *coldest = NULL;
    m68k_block **coldest_pp = NULL;
    u64 coldest_tag = (u64)-1;
    for (u32 i = 0; i < M68K_JIT_BLOCK_BUCKETS; i++) {
        m68k_block **pp = &d->buckets[i];
        while (*pp) {
            m68k_block *b = *pp;
            if (b->last_used_cycle < coldest_tag) {
                coldest_tag = b->last_used_cycle;
                coldest = b;
                coldest_pp = pp;
            }
            pp = &b->hash_next;
        }
    }
    if (!coldest) return 0;
    /* Unlink from the hash bucket. */
    *coldest_pp = coldest->hash_next;
    /* Other blocks may still point to it via predicted_next — invalidate
     * the whole predictor since we don't track reverse-edges. Cheaper
     * to clear all than to find which point at the victim. */
    for (u32 i = 0; i < M68K_JIT_BLOCK_BUCKETS; i++)
        for (m68k_block *b = d->buckets[i]; b; b = b->hash_next) {
            if (b->predicted_next == coldest) {
                b->predicted_next = NULL;
                b->predicted_next_pc = 0xFFFFFFFFu;
                b->predicted_next_entry = NULL;
            }
        }
    u32 offset = (u32)((u8 *)coldest->code - d->cc.base);
    u32 size = coldest->code_size;
    d->smc_invalidations++;
    m68k_block_free(coldest);
    /* Return the span to the codecache free list — but codecache_free
     * is a no-op in LRU mode for everyone except this caller path, so
     * we inline its effect by calling it directly. */
    codecache_free(&d->cc, offset, size);
    return size;
}

/* FIFO-mode eviction: every byte range that the ring is about to
 * overwrite must be evacuated. Drop every cached block whose codecache
 * span [code-base, code-base+code_size) overlaps [start, end). */
static void fifo_evict_range_cb(void *ctx, u32 start, u32 end) {
    m68k_dispatcher *d = (m68k_dispatcher *)ctx;
    int dropped_any = 0;
    for (u32 i = 0; i < M68K_JIT_BLOCK_BUCKETS; i++) {
        m68k_block **pp = &d->buckets[i];
        while (*pp) {
            m68k_block *b = *pp;
            u32 boff = (u32)((u8 *)b->code - d->cc.base);
            if (boff < end && boff + b->code_size > start) {
                *pp = b->hash_next;
                d->smc_invalidations++;
                m68k_block_free(b);
                dropped_any = 1;
            } else {
                pp = &b->hash_next;
            }
        }
    }
    if (dropped_any) {
        /* predicted_next links may now dangle; clear them all. */
        for (u32 i = 0; i < M68K_JIT_BLOCK_BUCKETS; i++)
            for (m68k_block *b = d->buckets[i]; b; b = b->hash_next) {
                b->predicted_next = NULL;
                b->predicted_next_pc = 0xFFFFFFFFu;
                b->predicted_next_entry = NULL;
            }
    }
}

bool m68k_dispatcher_init_ex(m68k_dispatcher *d, m68k_cpu *cpu,
                             u32 arena_kb, u8 evict_mode) {
    memset(d, 0, sizeof(*d));
    d->cpu = cpu;
    d->arena_cap = arena_kb * 1024u;
    mac_write_watch = smc_watch;
    mac_write_watch_ctx = d;
#if defined(ESP_PLATFORM)
    extern void *m68k_jit_arena_alloc(u32 bytes);
    d->arena = m68k_jit_arena_alloc(d->arena_cap);
#else
    d->arena = malloc(d->arena_cap);
#endif
    if (!d->arena) return false;
    codecache_init(&d->cc, (u8 *)d->arena, d->arena_cap, evict_mode);
    if (evict_mode == CC_MODE_LRU) {
        d->cc.evict = lru_evict_cb;
        d->cc.evict_ctx = d;
    } else if (evict_mode == CC_MODE_FIFO) {
        d->cc.evict_range = fifo_evict_range_cb;
        d->cc.evict_ctx = d;
    }
    return true;
}

bool m68k_dispatcher_init(m68k_dispatcher *d, m68k_cpu *cpu) {
    return m68k_dispatcher_init_ex(d, cpu, M68K_JIT_ARENA_KB, CC_MODE_BUMP);
}

void m68k_dispatcher_shutdown(m68k_dispatcher *d) {
#ifdef JIT_HELPER_HISTO
    {
        extern u32 m68k_helper_histo[65536];
        extern u32 m68k_helper_first_pc[65536];
        typedef struct { u32 op; u32 cnt; } he_t;
        he_t top[20] = {0};
        for (u32 op = 0; op < 65536; op++) {
            u32 c = m68k_helper_histo[op];
            if (c == 0) continue;
            for (int i = 0; i < 20; i++) {
                if (c > top[i].cnt) {
                    for (int j = 19; j > i; j--) top[j] = top[j-1];
                    top[i].op = op; top[i].cnt = c; break;
                }
            }
        }
        fprintf(stderr, "[helper-histo] top opcodes (op  count  first-pc):\n");
        for (int i = 0; i < 20 && top[i].cnt > 0; i++)
            fprintf(stderr, "  %04x  %8u  pc=%06x\n",
                    top[i].op, top[i].cnt, m68k_helper_first_pc[top[i].op]);
    }
#endif
    if (mac_write_watch_ctx == d) {
        mac_write_watch = NULL;
        mac_write_watch_ctx = NULL;
    }
    free_all_blocks(d);
#if !defined(ESP_PLATFORM)
    free(d->arena);
#endif
    d->arena = NULL;
}

/* --- block execution -------------------------------------------------- */

#if defined(ESP_PLATFORM)
/* Defined in port/esp32s3 — the CALL0<->windowed bridge. */
extern void m68k_enter_block(u8 *entry, m68k_cpu *cpu);

/* How many blocks the JIT may chain natively before falling back to the
 * dispatcher (to tick VIA and poll interrupts). Higher = fewer dispatcher
 * round-trips, but worse VIA/IRQ latency. 16 keeps timer latency under
 * ~1 ms even on busy code paths. */
#define M68K_JIT_CHAIN_BUDGET  16u

static void enter_block(m68k_dispatcher *d, m68k_block *b) {
    /* The block's epilogue may JX to predicted_next->entry directly
     * (chain) instead of returning here. Hand it the current block and
     * a chain budget so the chain knows when to break out for housekeeping. */
    d->cpu->current_block = b;
    d->cpu->chain_budget = M68K_JIT_CHAIN_BUDGET;
    m68k_enter_block(b->code + b->entry_off, d->cpu);
}
#else
typedef struct {
    m68k_cpu  *cpu;
    m68k_block *block;
    u8 stack_buf[0x800];
} sim_ctx;

static u8 *sim_translate(xt_sim *s, u32 addr) {
    sim_ctx *c = (sim_ctx *)s->user;
    if (addr >= HOST_CPU_BASE && addr < HOST_CPU_BASE + sizeof(m68k_cpu))
        return (u8 *)c->cpu + (addr - HOST_CPU_BASE);
    if (addr >= HOST_STACK_BASE && addr < HOST_STACK_BASE + sizeof(c->stack_buf))
        return c->stack_buf + (addr - HOST_STACK_BASE);
    if (c->cpu && c->cpu->mem && c->cpu->mem->ram) {
        u32 guest = addr - HOST_RAM_BASE;
        /* Mac Plus alias: RAM mirrors fill 0..0x3FFFFF when ram_size <
         * 0x400000. Match mac_read16's `addr & (ram_size-1)` handling. */
        if (guest < 0x400000u && (guest & (c->cpu->mem->ram_size - 1u)) < c->cpu->mem->ram_size)
            return c->cpu->mem->ram + (guest & (c->cpu->mem->ram_size - 1u));
        /* ROM at guest 0x400000-0x41FFFF (aliased to HOST_RAM_BASE+0x400000). */
        if (c->cpu->mem->rom && guest >= 0x400000u &&
            guest < 0x400000u + c->cpu->mem->rom_size)
            return c->cpu->mem->rom + (guest - 0x400000u);
    }
    if (addr < c->block->code_size)
        return c->block->code + addr;
    return NULL;
}

static u32 sim_read_literal(xt_sim *s, u32 addr) {
    sim_ctx *c = (sim_ctx *)s->user;
    if (addr + 4 > c->block->code_size) return 0;
    const u8 *p = c->block->code + addr;
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

static void sim_call(xt_sim *s, u32 fn_token) {
    sim_ctx *c = (sim_ctx *)s->user;
    switch ((literal_id)fn_token) {
        case HELPER_M68K_STEP:           m68k_step(c->cpu); break;
        case HELPER_JIT_ORI_B_MMIO:      m68k_jit_ori_b_mmio(c->cpu); break;
        case HELPER_JIT_BTST_B_MMIO:     m68k_jit_btst_b_mmio(c->cpu); break;
        case HELPER_JIT_MOVEM_L_POSTINC: m68k_jit_movem_l_postinc_to_regs(c->cpu); break;
        case HELPER_JIT_MOVEM_L_PREDEC:  m68k_jit_movem_l_predec_from_regs(c->cpu); break;
        case HELPER_JIT_MOVEM_W_TO_MEM:  m68k_jit_movem_w_to_mem(c->cpu); break;
        case HELPER_JIT_MOVEM_L_TO_MEM:  m68k_jit_movem_l_to_mem(c->cpu); break;
        case HELPER_JIT_MOVE_W_POSTINC_TO_DN: m68k_jit_move_w_postinc_to_dn(c->cpu); break;
        default: break;
    }
}

static void enter_block(m68k_dispatcher *d, m68k_block *b, u32 start_off) {
    /* Set current_block for parity with the ESP32 path. Host's xt_sim
     * runs one block per invocation, so native chaining isn't emitted
     * here, but keeping cpu state consistent helps any future host-side
     * code that wants to introspect the active block. */
    d->cpu->current_block = b;
    d->cpu->chain_budget = 1;  /* host: never chain — one block per sim_run */

    xt_sim s;
    xt_sim_init(&s, b->code, b->code_size);
    s.pc = start_off;
    /* M6.82 — when the dispatcher routes the chain hit to chain_entry_off
     * (partial-skip) or body_off (full-skip), the prologue ops that would
     * have set up a3/a14/a4..a7 are skipped. On the ESP32 chain-JX path
     * those registers are preserved naturally (a3 is callee-saved across
     * the JX, the others held by the predecessor); on the host sim each
     * block runs in a fresh xt_sim with all regs zeroed, so we have to
     * pre-load the equivalent state here in C. The host's pre-load cost
     * isn't counted in xt_instrs, so the host metric correctly reflects
     * the LX7-op savings the ESP32 path gets. */
    {
        u32 entry_off = b->entry_off;
        u32 body_off  = (u32)((const u8 *)b->body_addr        - (const u8 *)b->code);
        u32 chain_off = (u32)((const u8 *)b->chain_entry_addr - (const u8 *)b->code);
        if (start_off != entry_off) {
            s.a[3] = HOST_CPU_BASE;   /* R_CPU = a3, set by the l32r */
            if (start_off == body_off) {
                /* body_off: also pre-load R_SR + every active cache slot,
                 * because the cache_reload / sr_reload between
                 * chain_entry_off and body_off is also skipped. */
                if (b->sr_loaded) s.a[14] = (u32)d->cpu->sr;
                int active = (int)(b->cache_sig & 0xFu);
                for (int slot = 0; slot < active; slot++) {
                    int gi = (int)((b->cache_sig >> (4 + slot * 4)) & 0xFu);
                    if (gi < 8) s.a[4 + slot] = d->cpu->d[gi];
                    else        s.a[4 + slot] = d->cpu->a[gi - 8];
                }
            }
            (void)chain_off;   /* used only for documentation symmetry */
        }
    }
    s.translate = sim_translate;
    s.read_literal = sim_read_literal;
    s.call_thunk = sim_call;

    static sim_ctx ctx;
    ctx.cpu = d->cpu;
    ctx.block = b;
    memset(ctx.stack_buf, 0, sizeof(ctx.stack_buf));
    s.user = &ctx;

    s.a[0] = 0;                 /* RET sentinel */
    s.a[1] = HOST_STACK_TOP;

    xt_sim_run(&s, 1u << 22);
    d->xt_instrs   += s.instr_count;     /* native-Xtensa-cycle proxy   */
    d->helper_calls += b->helper_ops;    /* dynamic CALLX0→m68k_step cnt */
    if (s.status != XT_SIM_RETURNED) {
        fprintf(stderr, "[m68k-jit] block pc=%06X stopped status=%d sim_pc=%u\n",
                b->pc_start, (int)s.status, (unsigned)s.pc);
        d->cpu->halted = M68K_HALT_ILLEGAL;
    }
}
#endif

/* --- run loop --------------------------------------------------------- */

/* M6.71 / M6.72 — `remaining_depth` controls speculative recursion:
 *   `remaining_depth < 0` = real demand-driven compile (may arena-reset
 *                            on overflow + then may seed prefetch).
 *   `remaining_depth >= 0` = speculative prefetch step. Skips arena
 *                            resets so a prefetch never evicts the
 *                            just-compiled real block. The recursive
 *                            call decrements `remaining_depth` and
 *                            stops walking when it reaches 0. */
static m68k_block *get_block_impl(m68k_dispatcher *d, u32 pc,
                                  int remaining_depth) {
    bool prefetching = (remaining_depth >= 0);
    if (!d->no_cache) {
        m68k_block *b = find_block(d, pc);
        if (b) {
            if (prefetching) d->prefetch_hits++;
            return b;
        }
    }
    m68k_block *b = m68k_compile_block(&d->cc, d->cpu, pc, helper_addr, d->cpu);
    if (!b && !prefetching) {
        /* Arena full: wipe everything and retry once into a fresh
         * arena. Only on demand-driven calls — prefetch silently
         * gives up rather than risk evicting the block we just
         * compiled to satisfy the actual dispatcher entry. */
        free_all_blocks(d);
        codecache_reset(&d->cc);
        d->arena_resets++;
        b = m68k_compile_block(&d->cc, d->cpu, pc, helper_addr, d->cpu);
    }
    if (b) {
        if (d->no_cache) {
            /* Bench mode: never cache — caller frees after executing. */
        } else {
            insert_block(d, b);
            smc_mark_block(d, b);
        }
        d->blocks_compiled++;
        d->inline_ops_total += b->inline_ops;
        d->helper_ops_total += b->helper_ops;
        if (prefetching) d->prefetch_compiles++;
    }

    /* Prefetch step. Three modes:
     *   STATIC — every static successor (Bcc-both-branches included),
     *            single-step (no recursion past the first hop).
     *   CHAIN  — only single-successor blocks (skip Bcc / DBcc), but
     *            recurse to follow linear chains.
     *   NONE   — nothing. */
    if (!b || d->no_cache || d->prefetch_mode == PREFETCH_NONE) return b;

    /* For STATIC: seed only from a real-demand compile, single hop. */
    if (d->prefetch_mode == PREFETCH_STATIC && !prefetching) {
        u32 succ[2];
        int n = m68k_block_static_successors(b, d->cpu->mem, succ);
        for (int i = 0; i < n; i++) {
            if (succ[i] & 1u) continue;
            (void)get_block_impl(d, succ[i], /*remaining_depth=*/0);
        }
        return b;
    }

    /* For CHAIN: follow only single-successor blocks, recurse while
     * remaining_depth > 0. Seeded with depth = d->prefetch_depth on the
     * real-demand call. */
    if (d->prefetch_mode == PREFETCH_CHAIN) {
        int next_depth = prefetching ? (remaining_depth - 1)
                                     : (int)d->prefetch_depth - 1;
        if (next_depth < 0) return b;
        u32 succ[2];
        int n = m68k_block_static_successors(b, d->cpu->mem, succ);
        if (n != 1) return b;          /* skip Bcc / DBcc / dynamic */
        if (succ[0] & 1u) return b;
        (void)get_block_impl(d, succ[0], next_depth);
    }
    return b;
}

static m68k_block *get_block(m68k_dispatcher *d, u32 pc) {
    return get_block_impl(d, pc, /*remaining_depth=*/-1);
}

void m68k_dispatcher_set_prefetch(m68k_dispatcher *d, u8 mode, u8 depth) {
    d->prefetch_mode = mode;
    d->prefetch_depth = depth ? depth : 2;
}

void m68k_dispatcher_run_until(m68k_dispatcher *d, u64 until) {
    m68k_cpu *cpu = d->cpu;
    m68k_block *prev = NULL;

    while (cpu->cycles < until && !cpu->halted) {
        mac_mem_tick(cpu->mem, cpu->cycles);
        if (m68k_poll_interrupts(cpu)) { prev = NULL; continue; }
        if (sony_service(cpu)) { prev = NULL; continue; }
        if (cpu->stopped) { cpu->cycles += 64; continue; }

        u32 pc = cpu->pc;
        m68k_block *b;
        bool chain_hit = false;

        if (prev && prev->predicted_next && prev->predicted_next_pc == pc) {
            b = prev->predicted_next;
            d->chain_hits++;
            chain_hit = true;
            if (prev->cache_sig == b->cache_sig) d->chain_cache_matches++;
        } else {
            /* get_block may trigger an arena reset (free_all_blocks)
             * which leaves `prev` dangling. M6.63: LRU/FIFO eviction
             * during the compile inside get_block can also drop the
             * block `prev` points at — both bump smc_invalidations, so
             * snapshot both counters and null `prev` if either moved. */
            u64 resets_before = d->arena_resets;
            u64 inv_before    = d->smc_invalidations;
            b = get_block(d, pc);
            d->chain_misses++;
            if (d->arena_resets != resets_before ||
                d->smc_invalidations != inv_before) prev = NULL;
            if (prev && !d->no_cache) {
                prev->predicted_next = b;
                prev->predicted_next_pc = pc;
                /* M6.62 / M6.82 — pick the chain JX target along three tiers:
                 *
                 *   compat (cache_sig + SR match):  → body_addr (skip ALL of
                 *                                    prologue: ~5+ LX7 ops)
                 *   not compat (chain hit only):    → chain_entry_addr (skip
                 *                                    the first 2 prologue ops
                 *                                    that are redundant on
                 *                                    chain transitions —
                 *                                    R_CPU is callee-saved
                 *                                    and a0 was just reloaded
                 *                                    from OFF_JITRETPC; saves
                 *                                    2 LX7 ops per chain
                 *                                    on the bench's 96.7 %
                 *                                    no-cache-compat path)
                 *   (cold dispatch from this loop:    → entry_addr, full
                 *                                    prologue, picked when
                 *                                    `prev == NULL`)
                 *
                 * Compat = cache_sig match (a4..a7 hold correct values) AND
                 *          if b needs SR, prev must also have had SR loaded
                 *          (a14 holds the correct R_SR value). */
                bool compat = (prev->cache_sig == b->cache_sig) &&
                              (!b->sr_loaded || prev->sr_loaded);
                prev->predicted_next_entry = compat ? b->body_addr
                                                    : b->chain_entry_addr;
            }
        }

        if (!b) {
            /* Compilation failed outright — single-step the interpreter. */
            m68k_step(cpu);
            d->interp_fallbacks++;
            prev = NULL;
            continue;
        }

        /* M6.63 LRU tag — update on every dispatch, *not* on chain hits
         * inside the block (those don't pass through here). Means the
         * LRU eviction sees recent dispatcher entries; chained blocks
         * keep whatever tag they had when last dispatched. This is the
         * right approximation: a chained block that's never dispatcher-
         * entered must be on a chain rooted at a hot dispatcher entry,
         * which keeps the root hot — eviction follows the root. */
        b->last_used_cycle = cpu->cycles;

        /* M6.82 — on chain hits, start the sim at the precomputed
         * predicted_next_entry (body_addr or chain_entry_addr) so the
         * host metric reflects the same skip-prologue savings the ESP32
         * native-chain path gets. On chain miss (cold dispatch from this
         * loop), start at entry_off for the full prologue. */
        u32 start_off;
        if (chain_hit && prev->predicted_next_entry) {
            start_off = (u32)((const u8 *)prev->predicted_next_entry
                              - (const u8 *)b->code);
        } else {
            start_off = b->entry_off;
        }
        enter_block(d, b, start_off);
        d->blocks_executed++;

        if (d->no_cache) { m68k_block_free(b); prev = NULL; }
        else             { prev = b; }

        /* Drop any blocks the just-executed code overwrote (segment
         * loads, self-modifying code). Done here, between blocks, so a
         * block is never freed while it is executing. */
        if (d->n_dirty != 0 || d->smc_overflow) {
            if (smc_flush(d)) prev = NULL;
        }
    }
}
