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

/* CCOUNT-based run-loop profiler (cheap: one rsr per probe). Default on;
 * -DJIT_PROFILE=0 compiles it out. u32 deltas (CCOUNT wraps ~18s @240MHz —
 * far longer than any single probed region) accumulate into u64 counters. */
#ifndef JIT_PROFILE
#define JIT_PROFILE 1
#endif
#if defined(ESP_PLATFORM) && JIT_PROFILE
static inline u32 jit_ccount(void) { u32 v; __asm__ volatile("rsr.ccount %0":"=r"(v)); return v; }
#define PROF_T0()        u32 _pt = jit_ccount()
#define PROF_ADD(field)  do { d->field += (u32)(jit_ccount() - _pt); } while (0)
#else
#define PROF_T0()        ((void)0)
#define PROF_ADD(field)  ((void)0)
#endif

/* --- block-execution trace ring (diagnostic) -------------------------
 * Records the guest PC entered at each dispatcher iteration, plus whether
 * the entry was a native chain hit and the dispatcher's predicted-next PC.
 * Frozen the instant the guest first enters a wild low address (< 0x1000 —
 * the page-0/vector-table region it lands in after a bad branch/return), so
 * the post-mortem shows the exact block that jumped into the weeds. Read via
 * the board's 'e' command (paired with the m68k_exc_frozen ring). */
u32 m68k_blk_trace[64][3];   /* {pc, chain_hit | (pred_pc<<1), cycle} */
u32 m68k_blk_trace_n;
u32 m68k_blk_frozen[64][3];
u32 m68k_blk_frozen_n;
int m68k_blk_frozen_done;

/* A7-corruption catch: the block after which guest A7 first becomes wild
 * (outside guest RAM). With JIT_DBG_NOCHAIN this is the exact culprit block;
 * with chaining it is the chain root. Captured once. */
u32 m68k_a7w_pc, m68k_a7w_prev, m68k_a7w_new, m68k_a7w_sr, m68k_a7w_end;
u32 m68k_a7w_cyc;
int m68k_a7w_done;

/* SR-corruption catch: the block after which cpu->sr first holds bits that
 * are impossible on a 68000 (valid mask 0xA71F = T|S|IMASK|CCR). A corrupted
 * R_SR (host a14, clobbered by the CALL8 helper bridge and not reloaded) is
 * the prime suspect for the A7 derail. */
#define M68K_SR_VALID_MASK 0xA71Fu
u32 m68k_srw_pc, m68k_srw_prev, m68k_srw_new, m68k_srw_end, m68k_srw_cyc;
int m68k_srw_done;

/* --- literal-pool resolver -------------------------------------------- */

#if defined(ESP_PLATFORM)
/* CALL0 trampoline into the windowed reference interpreter. */
extern void m68k_step_call0(m68k_cpu *cpu);

/* CALL0 -> windowed bridge for the HELPER_JIT_* fast helpers.
 *
 * Those helpers are ordinary windowed C functions (entry/retw). A JIT
 * block runs under CALL0, so invoking them with a bare CALLX0 leaves the
 * window un-rotated; the helper's RETW then mis-rotates on the way out and
 * corrupts the block's live registers (notably R_CPU=a3), crashing a few
 * calls later. This bridge performs a proper CALL8 instead — exactly like
 * m68k_step_call0, but with the target taken from a8 so one bridge serves
 * every fast helper. Convention set up by emit_jit_fast_helper:
 *   a2 = cpu (the helper's sole argument)   a8 = helper function pointer
 *   a0 = return address back into the JIT block
 * CALL8 preserves the caller's a0..a7, so R_CPU(a3) and the register cache
 * (a4..a7) survive; a8..a15 are reloaded by the post-call sequence.
 *
 * Emitted as raw file-scope assembly, NOT a `naked` C function: GCC's
 * Xtensa backend still prepends `entry` to a naked function (the window
 * ABI is not per-function), which would defeat the whole point. */
__asm__(
    ".text\n"
    ".align 4\n"
    ".global m68k_jit_helper_bridge\n"
    ".type   m68k_jit_helper_bridge, @function\n"
"m68k_jit_helper_bridge:\n"
    "mov    a10, a2\n"   /* cpu -> windowed callee's a2 after CALL8 */
    "callx8 a8\n"        /* helper(cpu) */
    "jx     a0\n"        /* back into the JIT block */
    ".size  m68k_jit_helper_bridge, .-m68k_jit_helper_bridge\n"
);
extern void m68k_jit_helper_bridge(void);
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

/* M6.91 — byte-aligned bounds. Drops the `| 1` so any byte address in
 * RAM (or ROM) passes the AND fast-path. Used by the MOVE.B inline arms. */
static u32 ram_bounds_mask_byte(m68k_cpu *cpu) {
    if (!cpu || !cpu->mem) return 0xFFFFFFFFu;
    if (cpu->mem->overlay) return 0xFFFFFFFFu;
    u32 sz = cpu->mem->ram_size;
    if (sz == 0 || (sz & (sz - 1))) return 0xFFFFFFFFu;
    return ~(sz - 1u);
}
static u32 rom_bounds_mask_byte(m68k_cpu *cpu) {
    if (!cpu || !cpu->mem) return 0xFFFFFFFFu;
    if (cpu->mem->overlay) return 0xFFFFFFFFu;
    u32 sz = cpu->mem->rom_size;
    if (sz == 0 || (sz & (sz - 1))) return 0xFFFFFFFFu;
    return ~(sz - 1u);
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
        case HELPER_JIT_RTS_MMIO:        return (u32)(uintptr_t)&m68k_jit_rts_mmio;
        case HELPER_JIT_BSR_S_MMIO:      return (u32)(uintptr_t)&m68k_jit_bsr_s_mmio;
        case HELPER_JIT_BSR_W_MMIO:      return (u32)(uintptr_t)&m68k_jit_bsr_w_mmio;
        case HELPER_JIT_MOVE_L_POSTINC_TO_DN_MMIO: return (u32)(uintptr_t)&m68k_jit_move_l_postinc_to_dn_mmio;
        case HELPER_JIT_MOVE_B_ADDR_TO_DN_MMIO: return (u32)(uintptr_t)&m68k_jit_move_b_addr_to_dn_mmio;
        case HELPER_JIT_MOVE_B_DN_TO_ADDR_MMIO: return (u32)(uintptr_t)&m68k_jit_move_b_dn_to_addr_mmio;
        case HELPER_JIT_MOVE_L_DN_TO_ANPI_MMIO: return (u32)(uintptr_t)&m68k_jit_move_l_dn_to_anpi_mmio;
        case HELPER_JIT_MOVE_B_ADDR_TO_AN_MMIO: return (u32)(uintptr_t)&m68k_jit_move_b_addr_to_an_mmio;
        case HELPER_JIT_MOVE_B_IMM_TO_ADDR_MMIO: return (u32)(uintptr_t)&m68k_jit_move_b_imm_to_addr_mmio;
        case HELPER_JIT_FLINE_TRAP:      return (u32)(uintptr_t)&m68k_jit_fline_trap;
        case HELPER_JIT_MOVE_L_AN_TO_DN_MMIO: return (u32)(uintptr_t)&m68k_jit_move_l_an_to_dn_mmio;
        case HELPER_JIT_CLR_W_ANPI_MMIO: return (u32)(uintptr_t)&m68k_jit_clr_w_anpi_mmio;
        case HELPER_JIT_TST_B_MMIO:      return (u32)(uintptr_t)&m68k_jit_tst_b_mmio;
        case HELPER_JIT_ALINE_TRAP:      return (u32)(uintptr_t)&m68k_jit_aline_trap;
        case HELPER_JIT_MOVE_ANPI_TO_SR: return (u32)(uintptr_t)&m68k_jit_move_anpi_to_sr;
        case HELPER_JIT_RTE:             return (u32)(uintptr_t)&m68k_jit_rte;
        case HELPER_JIT_BITOP_DN_AN_MMIO: return (u32)(uintptr_t)&m68k_jit_bitop_dn_an_mmio;
        case HELPER_JIT_MOVE_L_XXXW_TO_AN_MMIO: return (u32)(uintptr_t)&m68k_jit_move_l_xxxw_to_an_mmio;
        case HELPER_JIT_MOVE_B_ADDR_TO_ADDR_MMIO: return (u32)(uintptr_t)&m68k_jit_move_b_addr_to_addr_mmio;
        case HELPER_JIT_MOVE_L_ADDR_TO_ADDR_MMIO: return (u32)(uintptr_t)&m68k_jit_move_l_addr_to_addr_mmio;
        case HELPER_JIT_MOVE_W_ADDR_TO_DN_MMIO: return (u32)(uintptr_t)&m68k_jit_move_w_addr_to_dn_mmio;
        case HELPER_JIT_MOVE_B_POSTINC_TO_DN_MMIO: return (u32)(uintptr_t)&m68k_jit_move_b_postinc_to_dn_mmio;
        case HELPER_JIT_MOVEA_L_ADDR_TO_AM_MMIO: return (u32)(uintptr_t)&m68k_jit_movea_l_addr_to_am_mmio;
        case HELPER_JIT_CMP_W_ADDR_DN_MMIO: return (u32)(uintptr_t)&m68k_jit_cmp_w_addr_dn_mmio;
        case HELPER_JIT_MOVE_W_ADDR_TO_POSTINC_MMIO: return (u32)(uintptr_t)&m68k_jit_move_w_addr_to_postinc_mmio;
        case HELPER_JIT_MOVEA_W_ADDR_TO_AM_MMIO: return (u32)(uintptr_t)&m68k_jit_movea_w_addr_to_am_mmio;
        case LITERAL_ROM_BOUNDS:return rom_bounds_mask(cpu);
        case LITERAL_ROM_BASE:  return (cpu && cpu->mem && cpu->mem->rom) ? MAC_ROM_BASE : 0xFFFFFFFFu;
        /* host_ptr - guest_base, so `host_ptr + guest_addr` lands at
         * the host's rom[] entry for the matching guest ROM address. */
        case ADDR_ROM_HOST_BASE:
            return (cpu && cpu->mem && cpu->mem->rom)
                ? ((u32)(uintptr_t)cpu->mem->rom - MAC_ROM_BASE)
                : 0u;
        case LITERAL_RAM_BOUNDS_BYTE: return ram_bounds_mask_byte(cpu);
        case LITERAL_ROM_BOUNDS_BYTE: return rom_bounds_mask_byte(cpu);
        case ADDR_HELPER_BRIDGE: return (u32)(uintptr_t)&m68k_jit_helper_bridge;
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
        case HELPER_JIT_RTS_MMIO:        return (u32)HELPER_JIT_RTS_MMIO;
        case HELPER_JIT_BSR_S_MMIO:      return (u32)HELPER_JIT_BSR_S_MMIO;
        case HELPER_JIT_BSR_W_MMIO:      return (u32)HELPER_JIT_BSR_W_MMIO;
        case HELPER_JIT_MOVE_L_POSTINC_TO_DN_MMIO: return (u32)HELPER_JIT_MOVE_L_POSTINC_TO_DN_MMIO;
        case HELPER_JIT_MOVE_B_ADDR_TO_DN_MMIO: return (u32)HELPER_JIT_MOVE_B_ADDR_TO_DN_MMIO;
        case HELPER_JIT_MOVE_B_DN_TO_ADDR_MMIO: return (u32)HELPER_JIT_MOVE_B_DN_TO_ADDR_MMIO;
        case HELPER_JIT_MOVE_L_DN_TO_ANPI_MMIO: return (u32)HELPER_JIT_MOVE_L_DN_TO_ANPI_MMIO;
        case HELPER_JIT_MOVE_B_ADDR_TO_AN_MMIO: return (u32)HELPER_JIT_MOVE_B_ADDR_TO_AN_MMIO;
        case HELPER_JIT_MOVE_B_IMM_TO_ADDR_MMIO: return (u32)HELPER_JIT_MOVE_B_IMM_TO_ADDR_MMIO;
        case HELPER_JIT_FLINE_TRAP:     return (u32)HELPER_JIT_FLINE_TRAP;
        case HELPER_JIT_MOVE_L_AN_TO_DN_MMIO: return (u32)HELPER_JIT_MOVE_L_AN_TO_DN_MMIO;
        case HELPER_JIT_CLR_W_ANPI_MMIO: return (u32)HELPER_JIT_CLR_W_ANPI_MMIO;
        case HELPER_JIT_TST_B_MMIO:      return (u32)HELPER_JIT_TST_B_MMIO;
        case HELPER_JIT_ALINE_TRAP:      return (u32)HELPER_JIT_ALINE_TRAP;
        case HELPER_JIT_MOVE_ANPI_TO_SR: return (u32)HELPER_JIT_MOVE_ANPI_TO_SR;
        case HELPER_JIT_RTE:             return (u32)HELPER_JIT_RTE;
        case HELPER_JIT_BITOP_DN_AN_MMIO: return (u32)HELPER_JIT_BITOP_DN_AN_MMIO;
        case HELPER_JIT_MOVE_L_XXXW_TO_AN_MMIO: return (u32)HELPER_JIT_MOVE_L_XXXW_TO_AN_MMIO;
        case HELPER_JIT_MOVE_B_ADDR_TO_ADDR_MMIO: return (u32)HELPER_JIT_MOVE_B_ADDR_TO_ADDR_MMIO;
        case HELPER_JIT_MOVE_L_ADDR_TO_ADDR_MMIO: return (u32)HELPER_JIT_MOVE_L_ADDR_TO_ADDR_MMIO;
        case HELPER_JIT_MOVE_W_ADDR_TO_DN_MMIO: return (u32)HELPER_JIT_MOVE_W_ADDR_TO_DN_MMIO;
        case HELPER_JIT_MOVE_B_POSTINC_TO_DN_MMIO: return (u32)HELPER_JIT_MOVE_B_POSTINC_TO_DN_MMIO;
        case HELPER_JIT_MOVEA_L_ADDR_TO_AM_MMIO: return (u32)HELPER_JIT_MOVEA_L_ADDR_TO_AM_MMIO;
        case HELPER_JIT_CMP_W_ADDR_DN_MMIO: return (u32)HELPER_JIT_CMP_W_ADDR_DN_MMIO;
        case HELPER_JIT_MOVE_W_ADDR_TO_POSTINC_MMIO: return (u32)HELPER_JIT_MOVE_W_ADDR_TO_POSTINC_MMIO;
        case HELPER_JIT_MOVEA_W_ADDR_TO_AM_MMIO: return (u32)HELPER_JIT_MOVEA_W_ADDR_TO_AM_MMIO;
        case LITERAL_ROM_BOUNDS:return rom_bounds_mask(cpu);
        case LITERAL_ROM_BASE:  return (cpu && cpu->mem && cpu->mem->rom) ? MAC_ROM_BASE : 0xFFFFFFFFu;
        /* The host sim's translate maps HOST_RAM_BASE + (0x400000..rom_top)
         * to mem->rom for the matching guest range, so we can re-use the
         * same sentinel base on host — the sim's auto-route handles ROM. */
        case ADDR_ROM_HOST_BASE:
            return (cpu && cpu->mem && cpu->mem->rom) ? HOST_RAM_BASE : 0u;
        case LITERAL_RAM_BOUNDS_BYTE: return ram_bounds_mask_byte(cpu);
        case LITERAL_ROM_BOUNDS_BYTE: return rom_bounds_mask_byte(cpu);
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

/* --- O(1) LRU list + reverse chain-edge bookkeeping ------------------- */
static void lru_unlink(m68k_dispatcher *d, m68k_block *b) {
    if (b->lru_prev) b->lru_prev->lru_next = b->lru_next;
    else if (d->lru_head == b) d->lru_head = b->lru_next;
    if (b->lru_next) b->lru_next->lru_prev = b->lru_prev;
    else if (d->lru_tail == b) d->lru_tail = b->lru_prev;
    b->lru_prev = b->lru_next = NULL;
}
static void lru_push_front(m68k_dispatcher *d, m68k_block *b) {
    b->lru_prev = NULL;
    b->lru_next = d->lru_head;
    if (d->lru_head) d->lru_head->lru_prev = b;
    d->lru_head = b;
    if (!d->lru_tail) d->lru_tail = b;
}
static inline void lru_touch(m68k_dispatcher *d, m68k_block *b) {
    if (d->lru_head == b) return;        /* already MRU */
    lru_unlink(d, b);
    lru_push_front(d, b);
}
/* Remove `p` from its current successor's pred_users list (if any). */
static void pred_edge_unlink(m68k_block *p) {
    if (p->pred_user_prev) p->pred_user_prev->pred_user_next = p->pred_user_next;
    else if (p->predicted_next && p->predicted_next->pred_users == p)
        p->predicted_next->pred_users = p->pred_user_next;
    if (p->pred_user_next) p->pred_user_next->pred_user_prev = p->pred_user_prev;
    p->pred_user_prev = p->pred_user_next = NULL;
}
/* Set p->predicted_next = b, maintaining the reverse-edge list. */
static void pred_edge_set(m68k_block *p, m68k_block *b) {
    if (p->predicted_next == b) return;
    if (p->predicted_next) pred_edge_unlink(p);
    p->predicted_next = b;
    p->pred_user_prev = NULL;
    p->pred_user_next = b->pred_users;
    if (b->pred_users) b->pred_users->pred_user_prev = p;
    b->pred_users = p;
}
/* Detach a block about to be freed: null every predictor pointing AT it
 * (O(degree)), remove it from its own successor's reverse list, and unlink
 * it from the LRU list. Leaves the hash unlink to the caller. */
static void block_detach(m68k_dispatcher *d, m68k_block *b) {
    for (m68k_block *u = b->pred_users; u; ) {
        m68k_block *nu = u->pred_user_next;
        u->predicted_next = NULL;
        u->predicted_next_pc = 0xFFFFFFFFu;
        u->predicted_next_entry = NULL;
        u->pred_user_prev = u->pred_user_next = NULL;
        u = nu;
    }
    b->pred_users = NULL;
    pred_edge_unlink(b);
    lru_unlink(d, b);
}

static void insert_block(m68k_dispatcher *d, m68k_block *b) {
    u32 bk = bucket_of(b->pc_start);
    b->hash_next = d->buckets[bk];
    d->buckets[bk] = b;
    if (d->cc.mode == CC_MODE_LRU) lru_push_front(d, b);
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
    /* Whole graph dropped — reset the LRU list heads (per-block lru/pred
     * links live in the now-freed structs; no per-block teardown needed). */
    d->lru_head = d->lru_tail = NULL;
}

/* --- L2 code-byte cache ----------------------------------------------
 * The IRAM execute-arena is ~40x too small for the Mac boot's hot working
 * set, so blocks thrash compile->evict->recompile (~73% of host cycles in
 * the relocation phase). Since a finished block is position-independent
 * (l32r/branches are PC-relative; the literal pool holds absolute VALUES
 * copied verbatim), its bytes can be re-copied into ANY fresh IRAM slot and
 * run correctly. The L2 store keeps those bytes in PSRAM so re-dispatch of an
 * evicted block RE-COPIES (memcpy + isync) instead of RE-COMPILING. Keyed by
 * pc_start; a content hash over the guest code guards against a self-modifying
 * guest rehydrating stale machine code. */
#if defined(ESP_PLATFORM)
extern void *m68k_jit_l2_arena(u32 *cap);
#else
/* Host: a malloc-backed arena so the L2 path is exercised off-target. */
static void *m68k_jit_l2_arena(u32 *cap) {
    static u8 *base; static u32 c;
    if (!base) { c = 8u*1024*1024; base = (u8 *)malloc(c); if (!base) c = 0; }
    if (cap) *cap = c;
    return base;
}
#endif

/* An L2 entry + its code bytes are stored contiguously in one span of the L2
 * codecache arena (PSRAM): [l2_entry][code_size bytes]. The arena is an LRU
 * byte cache (codecache CC_MODE_LRU with l2_evict_cb), so it holds the current
 * working set and evicts cold entries — bounding L2 to a fixed size rather than
 * the whole boot's code footprint. */
typedef struct l2_entry {
    u32 pc_start, pc_end;
    u32 content_hash;
    u32 code_size;
    u32 entry_off, body_off, chain_entry_off;
    u32 cache_sig;
    u32 n_ops, inline_ops, helper_ops;
    u32 last_op_pc;
    u16 last_op;
    u8  sr_loaded;
    u8 *bytes;                     /* = (u8*)e + sizeof(l2_entry)         */
    u32 span_off, span_size;       /* this entry's span in d->l2cc        */
    struct l2_entry *hash_next;
    struct l2_entry *l2_lru_prev, *l2_lru_next;
} l2_entry;

static u32 l2_hash_guest(m68k_cpu *cpu, u32 lo, u32 hi) {
    u32 h = 2166136261u;
    for (u32 a = lo; a < hi; a += 2)
        h = (h ^ (u32)mac_read16(cpu->mem, a)) * 16777619u;
    return h;
}

/* --- L2 recency list (MRU = head, coldest victim = tail) -------------- */
static void l2_lru_unlink(m68k_dispatcher *d, l2_entry *e) {
    if (e->l2_lru_prev) e->l2_lru_prev->l2_lru_next = e->l2_lru_next;
    else if (d->l2_lru_head == e) d->l2_lru_head = e->l2_lru_next;
    if (e->l2_lru_next) e->l2_lru_next->l2_lru_prev = e->l2_lru_prev;
    else if (d->l2_lru_tail == e) d->l2_lru_tail = e->l2_lru_prev;
    e->l2_lru_prev = e->l2_lru_next = NULL;
}
static void l2_lru_push_front(m68k_dispatcher *d, l2_entry *e) {
    e->l2_lru_prev = NULL; e->l2_lru_next = d->l2_lru_head;
    if (d->l2_lru_head) d->l2_lru_head->l2_lru_prev = e;
    d->l2_lru_head = e;
    if (!d->l2_lru_tail) d->l2_lru_tail = e;
}
static void l2_hash_unlink(m68k_dispatcher *d, l2_entry *e) {
    l2_entry **pp = &d->l2_buckets[bucket_of(e->pc_start)];
    while (*pp && *pp != e) pp = &(*pp)->hash_next;
    if (*pp) *pp = e->hash_next;
}

static l2_entry *l2_find(m68k_dispatcher *d, u32 pc) {
    if (!d->l2_on) return NULL;
    for (l2_entry *e = d->l2_buckets[bucket_of(pc)]; e; e = e->hash_next)
        if (e->pc_start == pc) return e;
    return NULL;
}

/* codecache evict callback for the L2 arena: drop the coldest L2 entry,
 * return its freed span size so codecache_alloc can retry. */
static u32 l2_evict_cb(void *ctx) {
    m68k_dispatcher *d = (m68k_dispatcher *)ctx;
    l2_entry *victim = d->l2_lru_tail;
    if (!victim) return 0;
    u32 off = victim->span_off, sz = victim->span_size;
    l2_hash_unlink(d, victim);
    l2_lru_unlink(d, victim);
    d->l2_evicts++;
    codecache_free(&d->l2cc, off, sz);   /* entry struct lives in this span */
    return sz;
}

/* Store a freshly-compiled block's bytes + metadata into L2 (once). The bytes
 * are read from the IRAM slot with 32-bit loads (IRAM is 32-bit-access-only). */
static void l2_publish(m68k_dispatcher *d, m68k_block *b) {
    if (!d->l2_on) return;
    if (l2_find(d, b->pc_start)) return;            /* already cached */
    u32 span = (u32)sizeof(l2_entry) + b->code_size;
    u8 *p = codecache_alloc(&d->l2cc, span);        /* may evict via l2_evict_cb */
    if (!p) { d->l2_full++; return; }
    l2_entry *e = (l2_entry *)p;
    e->bytes = p + sizeof(l2_entry);
    e->span_off = (u32)(p - d->l2cc.base);
    e->span_size = span;
    for (u32 i = 0; i < b->code_size; i += 4)
        *(u32 *)(e->bytes + i) = *(u32 *)(b->code + i);
    e->pc_start = b->pc_start; e->pc_end = b->pc_end;
    e->content_hash = b->content_hash;
    e->code_size = b->code_size;
    e->entry_off = b->entry_off;
    e->body_off = (u32)((const u8 *)b->body_addr - b->code);
    e->chain_entry_off = (u32)((const u8 *)b->chain_entry_addr - b->code);
    e->cache_sig = b->cache_sig;
    e->n_ops = b->n_ops; e->inline_ops = b->inline_ops; e->helper_ops = b->helper_ops;
    e->last_op = b->last_op; e->last_op_pc = b->last_op_pc;
    e->sr_loaded = b->sr_loaded;
    u32 bk = bucket_of(b->pc_start);
    e->hash_next = d->l2_buckets[bk];
    d->l2_buckets[bk] = e;
    l2_lru_push_front(d, e);
    d->l2_publishes++;
}

/* Try to re-instantiate the block at `pc` from L2 into a fresh IRAM slot,
 * skipping compilation. Returns the new (un-inserted) block, or NULL on miss/
 * stale/no-space. */
static m68k_block *l2_rehydrate(m68k_dispatcher *d, u32 pc) {
    l2_entry *e = l2_find(d, pc);
    if (!e) return NULL;
    /* Staleness guard: if the guest rewrote this code, the hash differs. */
    if (l2_hash_guest(d->cpu, e->pc_start, e->pc_end) != e->content_hash) {
        d->l2_stale++;
        l2_hash_unlink(d, e); l2_lru_unlink(d, e);
        codecache_free(&d->l2cc, e->span_off, e->span_size);
        return NULL;                                /* caller recompiles fresh */
    }
    l2_lru_unlink(d, e); l2_lru_push_front(d, e);   /* mark recently used */
    u8 *iram = codecache_alloc(&d->cc, e->code_size);   /* may evict (L2-safe) */
    if (!iram) return NULL;
    for (u32 i = 0; i < e->code_size; i += 4)
        *(u32 *)(iram + i) = *(u32 *)(e->bytes + i);
    codecache_finalize(&d->cc, iram + e->entry_off, e->code_size - e->entry_off);
    m68k_block *b = m68k_block_alloc();
    if (!b) { codecache_free(&d->cc, (u32)(iram - d->cc.base), e->code_size); return NULL; }
    b->pc_start = e->pc_start; b->pc_end = e->pc_end; b->n_ops = e->n_ops;
    b->code = iram; b->code_size = e->code_size; b->entry_off = e->entry_off;
    b->entry_addr = (void *)(iram + e->entry_off);
    b->body_addr  = (void *)(iram + e->body_off);
    b->chain_entry_addr = (void *)(iram + e->chain_entry_off);
    b->sr_loaded = e->sr_loaded; b->cache_sig = e->cache_sig;
    b->inline_ops = e->inline_ops; b->helper_ops = e->helper_ops;
    b->last_op = e->last_op; b->last_op_pc = e->last_op_pc;
    b->content_hash = e->content_hash;
    b->predicted_next = NULL; b->predicted_next_pc = 0xFFFFFFFFu; b->predicted_next_entry = NULL;
    b->last_used_cycle = d->cpu->cycles;
    b->hash_next = NULL;
    d->l2_rehydrates++;
    return b;
}

/* Drop the L2 entry for a guest pc (true self-modifying code). */
static void l2_drop(m68k_dispatcher *d, u32 pc) {
    if (!d->l2_on) return;
    l2_entry **pp = &d->l2_buckets[bucket_of(pc)];
    while (*pp) {
        l2_entry *e = *pp;
        if (e->pc_start == pc) {
            *pp = e->hash_next;
            l2_lru_unlink(d, e);
            codecache_free(&d->l2cc, e->span_off, e->span_size);
        } else {
            pp = &e->hash_next;
        }
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
    /* Record the EXACT written address (dedup), not just the page, so the
     * flush can invalidate only blocks whose code actually covers it. */
    for (int i = 0; i < d->n_dirty; i++)
        if (d->dirty_addrs[i] == addr) return;
    if (d->n_dirty < (int)(sizeof(d->dirty_addrs) / sizeof(d->dirty_addrs[0])))
        d->dirty_addrs[d->n_dirty++] = addr;
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
/* Zero the hotspot-gate counters for every PC slot a [lo,hi) guest range
 * covers when a block is dropped, forcing it to re-earn `compile_threshold`
 * interpreter passes before re-compiling.
 *
 * KEPT ON even with the L2 byte-cache: it is the admission throttle that bounds
 * the L1<->L2 churn. Measured: with it OFF, a tiny L1 (~600 blocks) vs a huge
 * working set means EVERY miss rehydrates (~70k/window) and the rehydrate
 * (memcpy + alloc + isync + calloc) becomes the bottleneck (~0.6 MHz). With it
 * ON, sporadically-used blocks stay interpreted and only genuinely-hot blocks
 * (re-proven >= threshold between evictions) rehydrate (~7k/window) — ~3x
 * faster. (Rehydrate is cheap PER EVENT, but not free at 10x the volume.) */
#ifndef JIT_RESET_HOTNESS_ON_EVICT
#define JIT_RESET_HOTNESS_ON_EVICT 1
#endif
static inline void hotness_reset_range(m68k_dispatcher *d, u32 lo, u32 hi) {
#if JIT_RESET_HOTNESS_ON_EVICT
    if (!d->hotness) return;
    for (u32 a = lo; a < hi; a += 2)
        d->hotness[(a >> 1) & d->hotness_mask] = 0;
#else
    (void)d; (void)lo; (void)hi;
#endif
}

static void smc_drop_range(m68k_dispatcher *d, u32 lo, u32 hi) {
    for (u32 i = 0; i < M68K_JIT_BLOCK_BUCKETS; i++) {
        m68k_block **pp = &d->buckets[i];
        while (*pp) {
            m68k_block *b = *pp;
            if (b->pc_start < hi && b->pc_end > lo) {
                *pp = b->hash_next;
                d->dbg_smc_w = lo; d->dbg_smc_pcs = b->pc_start;
                d->dbg_smc_pce = b->pc_end;
                hotness_reset_range(d, b->pc_start, b->pc_end);
                l2_drop(d, b->pc_start);   /* guest rewrote code — L2 copy is stale */
                block_detach(d, b);        /* unlink LRU + reverse chain edges */
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
        /* Drop only blocks whose [pc_start,pc_end) actually contains a
         * written byte — adjacent data writes in a shared code page leave
         * the loop's blocks cached (the whole point: no recompile thrash). */
        for (int i = 0; i < d->n_dirty; i++)
            smc_drop_range(d, d->dirty_addrs[i], d->dirty_addrs[i] + 1u);
    }
    d->n_dirty = 0;
    d->smc_overflow = false;

    /* Predicted-next pointers may now dangle — clear them all, including the
     * reverse-edge (pred_users) links so block_detach stays consistent. */
    for (u32 i = 0; i < M68K_JIT_BLOCK_BUCKETS; i++)
        for (m68k_block *b = d->buckets[i]; b; b = b->hash_next) {
            b->predicted_next = NULL;
            b->predicted_next_pc = 0xFFFFFFFFu;
            b->predicted_next_entry = NULL;
            b->pred_users = NULL;
            b->pred_user_next = b->pred_user_prev = NULL;
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
    PROF_T0();
    /* O(1) victim: the LRU tail (coldest dispatcher entry). */
    m68k_block *coldest = d->lru_tail;
    if (!coldest) { PROF_ADD(prof_evict); return 0; }
    /* Unlink from its hash bucket. */
    m68k_block **pp = &d->buckets[bucket_of(coldest->pc_start)];
    while (*pp && *pp != coldest) pp = &(*pp)->hash_next;
    if (*pp) *pp = coldest->hash_next;
    /* O(degree): null only the predictors that point AT the victim, and
     * remove it from the LRU + reverse-edge lists. (Was two O(N_blocks)
     * bucket sweeps — the dominant relocation-phase cost once recompiles
     * became cheap L2 rehydrates.) */
    block_detach(d, coldest);
    u32 offset = (u32)((u8 *)coldest->code - d->cc.base);
    u32 size = coldest->code_size;
    d->smc_invalidations++;
    d->lru_evictions++;
    hotness_reset_range(d, coldest->pc_start, coldest->pc_end);
    /* Do NOT drop the L2 byte copy — re-dispatch must rehydrate, not recompile. */
    m68k_block_free(coldest);
    codecache_free(&d->cc, offset, size);
    PROF_ADD(prof_evict);
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
                block_detach(d, b);          /* unlink LRU + reverse edges */
                m68k_block_free(b);
                dropped_any = 1;
            } else {
                pp = &b->hash_next;
            }
        }
    }
    if (dropped_any) {
        /* predicted_next links may now dangle; clear them all (+ reverse edges). */
        for (u32 i = 0; i < M68K_JIT_BLOCK_BUCKETS; i++)
            for (m68k_block *b = d->buckets[i]; b; b = b->hash_next) {
                b->predicted_next = NULL;
                b->predicted_next_pc = 0xFFFFFFFFu;
                b->predicted_next_entry = NULL;
                b->pred_users = NULL;
                b->pred_user_next = b->pred_user_prev = NULL;
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
    /* L2 byte-cache: a PSRAM backing store, managed as its own LRU byte arena
     * so evicted L1 blocks re-copy into IRAM instead of recompiling, and the
     * L2 itself evicts cold entries to stay within a fixed size. Harmless if
     * the PSRAM alloc fails (l2_on stays false → recompile as before). */
    {
        u32 l2cap = 0;
        void *l2base = m68k_jit_l2_arena(&l2cap);
        if (l2base && l2cap >= 64u * 1024u) {
            codecache_init(&d->l2cc, (u8 *)l2base, l2cap, CC_MODE_LRU);
            d->l2cc.evict = l2_evict_cb;
            d->l2cc.evict_ctx = d;
            d->l2_on = true;
        }
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
        /* M6.205 — widened from top-20 to top-40 to better surface
         * slow-path-conversion candidates (M6.204 left 0x09D1 absent
         * from top-20, but lower-fire candidates were also hidden).
         * The full 65536-entry sweep is unchanged; this only affects
         * the displayed cutoff. */
        typedef struct { u32 op; u32 cnt; } he_t;
        enum { HISTO_TOP_N = 40 };
        he_t top[HISTO_TOP_N] = {0};
        for (u32 op = 0; op < 65536; op++) {
            u32 c = m68k_helper_histo[op];
            if (c == 0) continue;
            for (int i = 0; i < HISTO_TOP_N; i++) {
                if (c > top[i].cnt) {
                    for (int j = HISTO_TOP_N - 1; j > i; j--) top[j] = top[j-1];
                    top[i].op = op; top[i].cnt = c; break;
                }
            }
        }
        fprintf(stderr, "[helper-histo] top opcodes (op  count  first-pc):\n");
        for (int i = 0; i < HISTO_TOP_N && top[i].cnt > 0; i++)
            fprintf(stderr, "  %04x  %8u  pc=%06x\n",
                    top[i].op, top[i].cnt, m68k_helper_first_pc[top[i].op]);
    }
#endif
    if (mac_write_watch_ctx == d) {
        mac_write_watch = NULL;
        mac_write_watch_ctx = NULL;
    }
    free_all_blocks(d);
    if (d->l2_on) {
        memset(d->l2_buckets, 0, sizeof(d->l2_buckets));
        d->l2_lru_head = d->l2_lru_tail = NULL;
        codecache_reset(&d->l2cc);   /* rewind the L2 arena (board keeps the PSRAM block) */
        d->l2_on = false;
    }
#if !defined(ESP_PLATFORM)
    free(d->arena);
#endif
    d->arena = NULL;
    free(d->hotness);
    d->hotness = NULL;
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

static void enter_block(m68k_dispatcher *d, m68k_block *b, u32 start_off) {
    /* The block's epilogue may JX to predicted_next->entry directly
     * (chain) instead of returning here. Hand it the current block and
     * a chain budget so the chain knows when to break out for housekeeping.
     *
     * start_off is the entry offset the shared run loop picked: entry_off
     * for a cold dispatch (full prologue), or the predecessor's
     * predicted_next_entry (body_addr / chain_entry_addr) on a chain hit,
     * to skip the redundant prologue ops — mirroring the host sim path. */
    d->cpu->current_block = b;
#if JIT_DBG_NOCHAIN
    /* Debug: disable native chaining so every block returns to the dispatcher
     * (exact per-block A7/PC tracing). Native-chain epilogue's `beqz budget`
     * falls through to FALLBACK when budget==0. */
    d->cpu->chain_budget = 0;
#else
    d->cpu->chain_budget = M68K_JIT_CHAIN_BUDGET;
#endif
    m68k_enter_block(b->code + start_off, d->cpu);
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
        case HELPER_JIT_RTS_MMIO:        m68k_jit_rts_mmio(c->cpu); break;
        case HELPER_JIT_BSR_S_MMIO:      m68k_jit_bsr_s_mmio(c->cpu); break;
        case HELPER_JIT_BSR_W_MMIO:      m68k_jit_bsr_w_mmio(c->cpu); break;
        case HELPER_JIT_MOVE_L_POSTINC_TO_DN_MMIO: m68k_jit_move_l_postinc_to_dn_mmio(c->cpu); break;
        case HELPER_JIT_MOVE_B_ADDR_TO_DN_MMIO: m68k_jit_move_b_addr_to_dn_mmio(c->cpu); break;
        case HELPER_JIT_MOVE_B_DN_TO_ADDR_MMIO: m68k_jit_move_b_dn_to_addr_mmio(c->cpu); break;
        case HELPER_JIT_MOVE_L_DN_TO_ANPI_MMIO: m68k_jit_move_l_dn_to_anpi_mmio(c->cpu); break;
        case HELPER_JIT_MOVE_B_ADDR_TO_AN_MMIO: m68k_jit_move_b_addr_to_an_mmio(c->cpu); break;
        case HELPER_JIT_MOVE_B_IMM_TO_ADDR_MMIO: m68k_jit_move_b_imm_to_addr_mmio(c->cpu); break;
        case HELPER_JIT_FLINE_TRAP:      m68k_jit_fline_trap(c->cpu); break;
        case HELPER_JIT_MOVE_L_AN_TO_DN_MMIO: m68k_jit_move_l_an_to_dn_mmio(c->cpu); break;
        case HELPER_JIT_CLR_W_ANPI_MMIO: m68k_jit_clr_w_anpi_mmio(c->cpu); break;
        case HELPER_JIT_TST_B_MMIO:      m68k_jit_tst_b_mmio(c->cpu); break;
        case HELPER_JIT_ALINE_TRAP:      m68k_jit_aline_trap(c->cpu); break;
        case HELPER_JIT_MOVE_ANPI_TO_SR: m68k_jit_move_anpi_to_sr(c->cpu); break;
        case HELPER_JIT_RTE:             m68k_jit_rte(c->cpu); break;
        case HELPER_JIT_BITOP_DN_AN_MMIO: m68k_jit_bitop_dn_an_mmio(c->cpu); break;
        case HELPER_JIT_MOVE_L_XXXW_TO_AN_MMIO: m68k_jit_move_l_xxxw_to_an_mmio(c->cpu); break;
        case HELPER_JIT_MOVE_B_ADDR_TO_ADDR_MMIO: m68k_jit_move_b_addr_to_addr_mmio(c->cpu); break;
        case HELPER_JIT_MOVE_L_ADDR_TO_ADDR_MMIO: m68k_jit_move_l_addr_to_addr_mmio(c->cpu); break;
        case HELPER_JIT_MOVE_W_ADDR_TO_DN_MMIO: m68k_jit_move_w_addr_to_dn_mmio(c->cpu); break;
        case HELPER_JIT_MOVE_B_POSTINC_TO_DN_MMIO: m68k_jit_move_b_postinc_to_dn_mmio(c->cpu); break;
        case HELPER_JIT_MOVEA_L_ADDR_TO_AM_MMIO: m68k_jit_movea_l_addr_to_am_mmio(c->cpu); break;
        case HELPER_JIT_CMP_W_ADDR_DN_MMIO: m68k_jit_cmp_w_addr_dn_mmio(c->cpu); break;
        case HELPER_JIT_MOVE_W_ADDR_TO_POSTINC_MMIO: m68k_jit_move_w_addr_to_postinc_mmio(c->cpu); break;
        case HELPER_JIT_MOVEA_W_ADDR_TO_AM_MMIO: m68k_jit_movea_w_addr_to_am_mmio(c->cpu); break;
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
    /* L2 fast path: re-instantiate an evicted block from its PSRAM byte copy
     * (cheap memcpy + isync) instead of recompiling (expensive). */
    bool rehy = false;
    m68k_block *b = NULL;
    if (!d->no_cache) { b = l2_rehydrate(d, pc); rehy = (b != NULL); }
    if (!b) b = m68k_compile_block(&d->cc, d->cpu, pc, helper_addr, d->cpu);
    if (!b && !prefetching) {
        /* Arena full: wipe everything and retry once into a fresh
         * arena. Only on demand-driven calls — prefetch silently
         * gives up rather than risk evicting the block we just
         * compiled to satisfy the actual dispatcher entry. The L2 store
         * survives the reset, so prefer a (cheap) rehydrate on the retry. */
        free_all_blocks(d);
        codecache_reset(&d->cc);
        d->arena_resets++;
        b = l2_rehydrate(d, pc);
        rehy = (b != NULL);
        if (!b) b = m68k_compile_block(&d->cc, d->cpu, pc, helper_addr, d->cpu);
    }
    if (b) {
        if (d->no_cache) {
            /* Bench mode: never cache — caller frees after executing. */
        } else {
            insert_block(d, b);
            smc_mark_block(d, b);
            if (!rehy) l2_publish(d, b);   /* store bytes for cheap re-instantiation */
        }
        if (!rehy) {
            d->blocks_compiled++;
            d->inline_ops_total += b->inline_ops;
            d->helper_ops_total += b->helper_ops;
        }
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

void m68k_dispatcher_set_compile_threshold(m68k_dispatcher *d, u32 n) {
    /* One byte per guest instruction slot. 2 M slots (2 MB) covers the full
     * 4 MB RAM pc-space (pc>>1 < 0x200000) with no aliasing — that's where
     * the OS streams code; ROM pcs alias low but ROM code is hot and cache-
     * resident anyway. Sized to stay within PSRAM alongside the 4 MB guest
     * RAM. The gate disables itself if the allocation fails. */
    if (n && !d->hotness) {
        /* 1 MB (was 2 MB) to leave PSRAM headroom for the L2 byte-cache.
         * pc>>1 for 4 MB RAM spans 0..0x1FFFFF, so a 1 MB map aliases at the
         * 2 MB guest-address boundary — tolerable now that a premature compile
         * from an alias is cheap (it publishes to L2 once, then rehydrates). */
        u32 slots = 0x100000u;             /* 1 M instruction slots (1 MB) */
        d->hotness = (u8 *)calloc(1, slots);
        if (!d->hotness) { d->compile_threshold = 0; return; }
        d->hotness_mask = slots - 1u;
    }
    d->compile_threshold = n;
}

/* Refill the shared JIT literal table (cpu->jit_lit). Cheap (LITERAL_COUNT
 * helper_addr() calls); done at each run_until entry so overlay-dependent
 * literals (RAM/ROM bounds) reflect the current memory map. l32i in generated
 * code loads from here. _Static_assert keeps jit_lit within l32i's reach. */
_Static_assert(offsetof(m68k_cpu, jit_lit) + (LITERAL_COUNT - 1) * 4u <= 1020u,
               "cpu->jit_lit out of l32i offset range");
static void jit_refill_literals(m68k_dispatcher *d) {
    for (u32 i = 0; i < LITERAL_COUNT; i++)
        d->cpu->jit_lit[i] = helper_addr((literal_id)i, d->cpu);
}

void m68k_dispatcher_run_until(m68k_dispatcher *d, u64 until) {
    m68k_cpu *cpu = d->cpu;
    m68k_block *prev = NULL;
    jit_refill_literals(d);

    while (cpu->cycles < until && !cpu->halted) {
        { PROF_T0(); mac_mem_tick(cpu->mem, cpu->cycles); PROF_ADD(prof_tick); }
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
            /* Hotspot gate: interpret a not-yet-cached block until its
             * entry pc has been seen `compile_threshold` times. Skips
             * compiling run-once code (the bulk of an OS boot), which on a
             * small code cache would thrash compile+evict at ~100% miss. */
            if (d->compile_threshold && d->hotness && !d->no_cache &&
                !find_block(d, pc)) {
                u32 hi = (pc >> 1) & d->hotness_mask;
                if (d->hotness[hi] < d->compile_threshold) {
                    if (d->hotness[hi] != 0xFFu) d->hotness[hi]++;
                    { PROF_T0(); m68k_step(cpu); PROF_ADD(prof_interp); }
                    d->interp_fallbacks++;
                    prev = NULL;
                    continue;
                }
                /* Gate passed for an uncached pc → a fresh compile follows.
                 * If hotness is already at/over the cap (0xFF) the gate did
                 * NOT interpret-first — the prime suspect for rewrite-thrash:
                 * a previously-hot pc whose code was rewritten recompiles
                 * immediately because hotness was never reset. */
                if (d->hotness[hi] >= 0xFFu) d->hot_bypass++;
                else                         d->hot_gated++;
            }
            { PROF_T0(); b = get_block(d, pc); PROF_ADD(prof_compile); }
            d->chain_misses++;
            if (d->arena_resets != resets_before ||
                d->smc_invalidations != inv_before) prev = NULL;
            if (prev && !d->no_cache) {
                pred_edge_set(prev, b);   /* maintains the reverse-edge list */
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
                (void)compat;
                /* The native-chain JX enters the successor at its FULL
                 * prologue, which reloads R_CPU + the guest-register cache
                 * (a4..a7) + R_SR from the cpu state the predecessor's
                 * epilogue just flushed. The M6.82 skip-prologue targets
                 * (body_addr / chain_entry_addr) assumed those registers stay
                 * live across the inter-block JX — true in the host sim (which
                 * re-loads them in C) but NOT on real Xtensa, where the
                 * chained block then ran on stale registers and the boot hung.
                 * Full-prologue entry still skips the expensive dispatcher
                 * round-trip (tick / poll / hash lookup); only a few register
                 * reloads are re-done. */
                prev->predicted_next_entry = b->entry_addr;
            }
        }

        if (!b) {
            /* Compilation failed outright — single-step the interpreter. */
            m68k_step(cpu);
            d->interp_fallbacks++;
            prev = NULL;
            continue;
        }

#if JIT_DBG_TRACE
        /* Block-trace ring: record this dispatcher entry. (Chained blocks
         * that JX within an epilogue don't pass through here, but a block
         * that computes a bad target returns to the dispatcher — its bad
         * pc shows up as the *next* entry, with the culprit just before.) */
        {
            u32 ti = m68k_blk_trace_n & 63;
            m68k_blk_trace[ti][0] = pc;
            m68k_blk_trace[ti][1] = (u32)(chain_hit ? 1u : 0u) | (b->pc_end << 1);
            m68k_blk_trace[ti][2] = (u32)cpu->cycles;
            m68k_blk_trace_n++;
            if (!m68k_blk_frozen_done && pc < 0x1000u) {
                for (int k = 0; k < 64; k++) {
                    m68k_blk_frozen[k][0] = m68k_blk_trace[k][0];
                    m68k_blk_frozen[k][1] = m68k_blk_trace[k][1];
                    m68k_blk_frozen[k][2] = m68k_blk_trace[k][2];
                }
                m68k_blk_frozen_n = m68k_blk_trace_n;
                m68k_blk_frozen_done = 1;
            }
        }
#endif /* JIT_DBG_TRACE */

        /* M6.63 LRU tag — update on every dispatch, *not* on chain hits
         * inside the block (those don't pass through here). Means the
         * LRU eviction sees recent dispatcher entries; chained blocks
         * keep whatever tag they had when last dispatched. This is the
         * right approximation: a chained block that's never dispatcher-
         * entered must be on a chain rooted at a hot dispatcher entry,
         * which keeps the root hot — eviction follows the root. */
        b->last_used_cycle = cpu->cycles;
        if (d->cc.mode == CC_MODE_LRU) lru_touch(d, b);

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
#if defined(ESP_PLATFORM)
        /* On the ESP/Xtensa target, m68k_enter_block enters the block via a
         * cold CALL0 and does NOT pre-load the guest-register cache (a4..a7)
         * or R_SR (a14) — unlike the host sim's enter_block, which restores
         * them in C for the skip-prologue case. So a *dispatcher* entry must
         * always run the full prologue, which reloads that state from cpu.
         * The skip-prologue targets (body_addr / chain_entry_addr held in
         * predicted_next_entry) are valid ONLY on the native-chain JX in the
         * block epilogue, where a4..a7/a14 are still live from the just-
         * executed predecessor block. Entering them from here would run the
         * body against stale registers (manifested as a guest loop counter
         * that never converges). */
        start_off = b->entry_off;
#endif
#if JIT_DBG_TRACE
        u32 a7_pre = cpu->a[7];
        u32 sr_pre = cpu->sr;
#endif
        { PROF_T0(); enter_block(d, b, start_off); PROF_ADD(prof_exec); }
        d->blocks_executed++;

#if JIT_DBG_TRACE
        /* Catch the block that first puts impossible bits into cpu->sr. */
        if (!m68k_srw_done && (cpu->sr & ~M68K_SR_VALID_MASK) != 0 &&
            (sr_pre & ~M68K_SR_VALID_MASK) == 0) {
            m68k_srw_pc   = pc;
            m68k_srw_end  = b->pc_end;
            m68k_srw_prev = sr_pre;
            m68k_srw_new  = cpu->sr;
            m68k_srw_cyc  = (u32)cpu->cycles;
            m68k_srw_done = 1;
        }

        /* Catch the moment guest A7 first goes wild (outside RAM) — the
         * block that just ran (or the chain rooted here) corrupted it. */
        if (!m68k_a7w_done && cpu->a[7] >= cpu->mem->ram_size &&
            a7_pre < cpu->mem->ram_size) {
            m68k_a7w_pc   = pc;
            m68k_a7w_end  = b->pc_end;
            m68k_a7w_prev = a7_pre;
            m68k_a7w_new  = cpu->a[7];
            m68k_a7w_sr   = cpu->sr;
            m68k_a7w_cyc  = (u32)cpu->cycles;
            m68k_a7w_done = 1;
            /* Snapshot the block trace here too (A7 goes wild a block or two
             * before pc reaches 0), so b#trace shows the run-up. */
            if (!m68k_blk_frozen_done) {
                for (int k = 0; k < 64; k++) {
                    m68k_blk_frozen[k][0] = m68k_blk_trace[k][0];
                    m68k_blk_frozen[k][1] = m68k_blk_trace[k][1];
                    m68k_blk_frozen[k][2] = m68k_blk_trace[k][2];
                }
                m68k_blk_frozen_n = m68k_blk_trace_n;
                m68k_blk_frozen_done = 1;
            }
        }
#endif /* JIT_DBG_TRACE */

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
