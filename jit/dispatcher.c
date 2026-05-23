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
        }
    return true;
}

/* --- init / shutdown -------------------------------------------------- */

bool m68k_dispatcher_init(m68k_dispatcher *d, m68k_cpu *cpu) {
    memset(d, 0, sizeof(*d));
    d->cpu = cpu;
    d->arena_cap = M68K_JIT_ARENA_KB * 1024u;
    mac_write_watch = smc_watch;
    mac_write_watch_ctx = d;
#if defined(ESP_PLATFORM)
    extern void *m68k_jit_arena_alloc(u32 bytes);
    d->arena = m68k_jit_arena_alloc(d->arena_cap);
#else
    d->arena = malloc(d->arena_cap);
#endif
    if (!d->arena) return false;
    codecache_init(&d->cc, (u8 *)d->arena, d->arena_cap);
    return true;
}

void m68k_dispatcher_shutdown(m68k_dispatcher *d) {
#ifdef JIT_HELPER_HISTO
    {
        extern u32 m68k_helper_histo[65536];
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
        fprintf(stderr, "[helper-histo] top opcodes:\n");
        for (int i = 0; i < 20 && top[i].cnt > 0; i++)
            fprintf(stderr, "  %04x  %u\n", top[i].op, top[i].cnt);
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

static void enter_block(m68k_dispatcher *d, m68k_block *b) {
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
        default: break;
    }
}

static void enter_block(m68k_dispatcher *d, m68k_block *b) {
    xt_sim s;
    xt_sim_init(&s, b->code, b->code_size);
    s.pc = b->entry_off;
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

static m68k_block *get_block(m68k_dispatcher *d, u32 pc) {
    if (!d->no_cache) {
        m68k_block *b = find_block(d, pc);
        if (b) return b;
    }
    m68k_block *b = m68k_compile_block(&d->cc, d->cpu, pc, helper_addr, d->cpu);
    if (!b) {
        /* Arena full: wipe everything and retry once into a fresh arena. */
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
    }
    return b;
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

        if (prev && prev->predicted_next && prev->predicted_next_pc == pc) {
            b = prev->predicted_next;
            d->chain_hits++;
        } else {
            /* get_block may trigger an arena reset (free_all_blocks)
             * which leaves `prev` dangling. Snapshot the reset counter
             * and null `prev` if it changed before we touch it. */
            u64 resets_before = d->arena_resets;
            b = get_block(d, pc);
            d->chain_misses++;
            if (d->arena_resets != resets_before) prev = NULL;
            if (prev && !d->no_cache) {
                prev->predicted_next = b;
                prev->predicted_next_pc = pc;
            }
        }

        if (!b) {
            /* Compilation failed outright — single-step the interpreter. */
            m68k_step(cpu);
            d->interp_fallbacks++;
            prev = NULL;
            continue;
        }

        enter_block(d, b);
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
