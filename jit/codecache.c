#include "codecache.h"
#include <string.h>

#define CC_NIL    0xFFFFFFFFu
#define CC_ALIGN  8u                    /* alloc + span granularity */

static inline u32 cc_align(u32 v) { return (v + (CC_ALIGN - 1u)) & ~(CC_ALIGN - 1u); }

#if GBJIT_JIT_EVICT == 1
/* Intrusive free-span header, stored at the start of every free span. */
typedef struct { u32 size; u32 next; } cc_free_hdr;
#endif

void codecache_init(codecache *cc, u8 *base, u32 cap) {
    cc->base = base;
    cc->used = 0;
    cc->cap  = cap & ~(CC_ALIGN - 1u);
    cc->free_head = CC_NIL;
    cc->evict = 0;
    cc->evict_range = 0;
    cc->evict_ctx = 0;
#if GBJIT_JIT_EVICT == 1
    /* Coldness mode: whole arena starts as one free span. */
    if (cc->cap >= sizeof(cc_free_hdr)) {
        cc_free_hdr *h = (cc_free_hdr *)cc->base;
        h->size = cc->cap;
        h->next = CC_NIL;
        cc->free_head = 0;
    }
#endif
}

#if GBJIT_JIT_EVICT == 1   /* ---- coldness: free-list allocator ---- */

u8 *codecache_alloc(codecache *cc, u32 size) {
    size = cc_align(size);
    if (size < sizeof(cc_free_hdr)) size = sizeof(cc_free_hdr);
    if (size > cc->cap) return NULL;

    for (;;) {
        /* First-fit walk of the address-sorted free list. */
        u32 prev = CC_NIL, cur = cc->free_head;
        while (cur != CC_NIL) {
            cc_free_hdr *h = (cc_free_hdr *)(cc->base + cur);
            if (h->size >= size) {
                u32 leftover = h->size - size;        /* 8-aligned: 0 or >=8 */
                if (leftover >= sizeof(cc_free_hdr)) {
                    /* Carve `size` off the front; the tail stays free. */
                    u32 toff = cur + size;
                    cc_free_hdr *t = (cc_free_hdr *)(cc->base + toff);
                    t->size = leftover;
                    t->next = h->next;
                    if (prev == CC_NIL) cc->free_head = toff;
                    else ((cc_free_hdr *)(cc->base + prev))->next = toff;
                } else {
                    /* Exact fit — unlink the whole span. */
                    if (prev == CC_NIL) cc->free_head = h->next;
                    else ((cc_free_hdr *)(cc->base + prev))->next = h->next;
                }
                return cc->base + cur;
            }
            prev = cur;
            cur = h->next;
        }
        /* Nothing fit — evict the coldest cached block and retry. */
        if (!cc->evict || cc->evict(cc->evict_ctx) == 0) return NULL;
    }
}

void codecache_free(codecache *cc, u32 offset, u32 size) {
    size = cc_align(size);
    if (size < sizeof(cc_free_hdr)) size = sizeof(cc_free_hdr);

    /* Insert into the address-sorted list: prev < offset <= cur. */
    u32 prev = CC_NIL, cur = cc->free_head;
    while (cur != CC_NIL && cur < offset) {
        prev = cur;
        cur = ((cc_free_hdr *)(cc->base + cur))->next;
    }
    cc_free_hdr *nh = (cc_free_hdr *)(cc->base + offset);
    nh->size = size;
    nh->next = cur;
    if (prev == CC_NIL) cc->free_head = offset;
    else ((cc_free_hdr *)(cc->base + prev))->next = offset;

    /* Coalesce with the following span, then with the preceding one. */
    if (cur != CC_NIL && offset + nh->size == cur) {
        cc_free_hdr *ch = (cc_free_hdr *)(cc->base + cur);
        nh->size += ch->size;
        nh->next  = ch->next;
    }
    if (prev != CC_NIL) {
        cc_free_hdr *ph = (cc_free_hdr *)(cc->base + prev);
        if (prev + ph->size == offset) {
            ph->size += nh->size;
            ph->next  = nh->next;
        }
    }
}

void codecache_reset(codecache *cc) {
    cc->used = 0;
    cc->free_head = CC_NIL;
    if (cc->cap >= sizeof(cc_free_hdr)) {
        cc_free_hdr *h = (cc_free_hdr *)cc->base;
        h->size = cc->cap;
        h->next = CC_NIL;
        cc->free_head = 0;
    }
}

void codecache_trim(codecache *cc, u8 *block, u32 actual, u32 reserved) {
    actual = cc_align(actual);
    reserved = cc_align(reserved);
    /* Return the unused tail of this allocation to the free list. */
    if (reserved > actual)
        codecache_free(cc, (u32)(block - cc->base) + actual, reserved - actual);
}

#elif GBJIT_JIT_EVICT == 2   /* ---- circular ring: FIFO eviction ---- */

u8 *codecache_alloc(codecache *cc, u32 size) {
    size = cc_align(size);
    if (size > cc->cap) return NULL;
    /* `used` is the ring write cursor. Wrap rather than split a block
     * across the cap boundary. */
    if (cc->used + size > cc->cap) cc->used = 0;
    u32 start = cc->used;
    /* Evict every block whose bytes [start, start+size) is about to
     * overwrite — always the oldest-compiled run, since the ring fills
     * in compile order. */
    if (cc->evict_range) cc->evict_range(cc->evict_ctx, start, start + size);
    cc->used += size;
    return cc->base + start;
}

void codecache_free(codecache *cc, u32 offset, u32 size) {
    (void)cc; (void)offset; (void)size;     /* ring reclaims by overwrite */
}

void codecache_reset(codecache *cc) {
    cc->used = 0;
}

void codecache_trim(codecache *cc, u8 *block, u32 actual, u32 reserved) {
    (void)reserved;
    /* Pull the ring cursor back to the block's real end. */
    cc->used = (u32)(block - cc->base) + cc_align(actual);
}

#else  /* GBJIT_JIT_EVICT == 0 — plain bump allocator */

u8 *codecache_alloc(codecache *cc, u32 size) {
    cc->used = cc_align(cc->used);
    if (cc->used + size > cc->cap) return NULL;
    u8 *p = cc->base + cc->used;
    cc->used += size;
    return p;
}

void codecache_free(codecache *cc, u32 offset, u32 size) {
    (void)cc; (void)offset; (void)size;     /* bump allocator can't reclaim */
}

void codecache_reset(codecache *cc) {
    cc->used = 0;
}

void codecache_trim(codecache *cc, u8 *block, u32 actual, u32 reserved) {
    (void)reserved;
    /* The block is the most recent allocation, so the bump cursor sits
     * right past it — pull it back to the block's real end and the next
     * compile packs in immediately after. This is the whole density
     * win: blocks reserve a worst-case size but emit ~half of it. */
    cc->used = (u32)(block - cc->base) + cc_align(actual);
}

#endif

#if defined(ESP_PLATFORM)
/* ESP32-S3 IRAM is internal SRAM that is NOT routed through the L1 cache
 * (cache covers flash/PSRAM only). Writes via the data path are visible to
 * the instruction fetch path immediately. We still emit a memory barrier
 * so the compiler can't reorder later instruction fetches above the code-
 * writing stores. */
void codecache_finalize(codecache *cc, u8 *block, u32 size) {
    (void)cc; (void)block; (void)size;
    __sync_synchronize();
}
#else
void codecache_finalize(codecache *cc, u8 *block, u32 size) {
    (void)cc; (void)block; (void)size;
    /* Host x86_64: writes are coherent with icache. */
    __builtin___clear_cache((char *)block, (char *)(block + size));
}
#endif
