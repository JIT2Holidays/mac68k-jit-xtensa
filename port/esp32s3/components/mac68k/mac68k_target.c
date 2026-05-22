/* ESP32-S3 target glue for the JIT: the executable codecache arena and the
 * windowed -> CALL0 entry bridge into a compiled block.
 *
 * Only compiled for the ESP-IDF build (dispatcher.c calls these under
 * #ifdef ESP_PLATFORM). The host build runs blocks through xtensa_sim.c. */

#include "m68k_types.h"
#include "m68k_cpu.h"
#include "esp_heap_caps.h"
#include <stdint.h>

/* The codecache must live in IRAM and be executable. */
void *m68k_jit_arena_alloc(u32 bytes) {
    void *p = heap_caps_malloc(bytes,
        MALLOC_CAP_EXEC | MALLOC_CAP_32BIT | MALLOC_CAP_INTERNAL);
    return p;
}

/* Enter a compiled block.
 *
 * The block is CALL0 code; this function is windowed. The frame-padding
 * trick (`pad[]`) is load-bearing — see gbjit-xtensa's enter_block_native
 * for the full rationale: it guarantees `a1 + 0` sits in our own private
 * frame space, below both the windowed caller's register-spill area and
 * any helper frame, so stashing the block's return PC there survives a
 * window overflow triggered deep inside a helper call.
 *
 * `a1` is never modified. The block's prologue loads cpu_state from its
 * literal pool, so it does not need an argument register — but we pin a2
 * to cpu defensively. */
__attribute__((noinline))
void m68k_enter_block(u8 *entry, m68k_cpu *cpu) {
    volatile uint32_t pad[12];
    uint32_t fn = (uint32_t)(uintptr_t)entry;
    pad[0] = fn;
    register uint32_t a2_cpu asm("a2") = (uint32_t)(uintptr_t)cpu;
    register uint32_t a8_fn  asm("a8") = fn;
    asm volatile (
        "s32i a0, a1, 0\n"      /* stash windowed return PC at frame bottom */
        "callx0 %1\n"           /* CALL0 into the JIT block */
        "l32i a0, a1, 0\n"      /* restore windowed return PC */
        : "+r"(a2_cpu)
        : "r"(a8_fn)
        : "a3","a4","a5","a6","a7","a9","a10","a11","a12","a13","a14","a15",
          "memory"
    );
    (void)pad;
}
