#ifndef CODECACHE_H
#define CODECACHE_H

#include "m68k_types.h"

/* Single-arena code cache. The host port allocates the backing memory with
   PROT_READ|PROT_WRITE|PROT_EXEC; the ESP32-S3 port uses heap_caps_malloc
   with MALLOC_CAP_EXEC|MALLOC_CAP_INTERNAL.

   Three allocators, picked at compile time by GBJIT_JIT_EVICT:

   - 0 (default): bump allocator. codecache_alloc fails (NULL) once the
     arena is full; the dispatcher then interp-falls-back for new PCs.
     A working set larger than the arena hits a hard performance cliff.

   - 1: free-list allocator with coldness eviction. When no free span
     fits, the dispatcher's evict callback drops the *coldest* cached
     block (lowest last-use tag, an approximate-LRU choice) and returns
     its span. Per-block 8-byte tag; an O(blocks) victim scan + a
     free-list walk per compile; suffers free-list fragmentation.

   - 2: circular ring with FIFO eviction. The write cursor wraps; a new
     block overwrites — and so evicts — whichever blocks its bytes land
     on, always the oldest-compiled ones. No tag, no free list, no
     fragmentation; but it cannot protect a hot block that happens to
     sit under the cursor. */

#ifndef GBJIT_JIT_EVICT
#define GBJIT_JIT_EVICT 0
#endif

/* Coldness mode (=1): evict one victim, return bytes returned via
   codecache_free, or 0 if nothing could be evicted. */
typedef u32 (*cc_evict_fn)(void *ctx);

/* Circular mode (=2): evict every cached block overlapping the arena
   byte range [start, end) — about to be overwritten by the ring. */
typedef void (*cc_evict_range_fn)(void *ctx, u32 start, u32 end);

typedef struct {
    u8  *base;
    u32  used;            /* bump mode: high-water mark; ring mode: cursor */
    u32  cap;
    /* Coldness mode (GBJIT_JIT_EVICT=1). Intrusive free spans: an 8-byte
       {size,next} header at the start of each free span; address-sorted. */
    u32  free_head;       /* offset of first free span, or CC_NIL */
    cc_evict_fn       evict;        /* coldness mode */
    cc_evict_range_fn evict_range;  /* circular mode */
    void *evict_ctx;
} codecache;

void codecache_init(codecache *cc, u8 *base, u32 cap);

/* Reserve `size` bytes; returns pointer or NULL (bump mode full / request
   exceeds the whole arena). The region is uninitialised; caller writes
   machine code then calls codecache_finalize. */
u8 *codecache_alloc(codecache *cc, u32 size);

/* Coldness mode: return a [offset, offset+size) span to the free list
   (coalescing). No-op in bump and circular modes. */
void codecache_free(codecache *cc, u32 offset, u32 size);

/* Shrink the most-recent allocation in place. codecache_alloc must
   reserve a worst-case size before the block is emitted; once the real
   size is known the caller trims the slack back. `block` is the alloc's
   returned pointer, `actual` the bytes actually used, `reserved` what
   was requested. Caller must not have allocated again in between. */
void codecache_trim(codecache *cc, u8 *block, u32 actual, u32 reserved);

/* Make written code visible to the icache. */
void codecache_finalize(codecache *cc, u8 *block, u32 size);

void codecache_reset(codecache *cc);

#endif
