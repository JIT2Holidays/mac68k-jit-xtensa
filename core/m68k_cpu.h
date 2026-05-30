#ifndef M68K_CPU_H
#define M68K_CPU_H

#include "m68k_types.h"

/* Condition code register bits (low byte of SR). */
#define CCR_C 0x01u
#define CCR_V 0x02u
#define CCR_Z 0x04u
#define CCR_N 0x08u
#define CCR_X 0x10u

/* Status register high-byte bits. */
#define SR_S    0x2000u   /* supervisor mode  */
#define SR_T    0x8000u   /* trace            */
#define SR_IMASK 0x0700u  /* interrupt mask   */

struct mac_mem;

/* Reasons the run loop stops (cpu->halted is set non-zero). */
enum {
    M68K_RUN = 0,
    M68K_HALT_RESET,      /* RESET instruction or guest requested exit */
    M68K_HALT_DOUBLE_FAULT,
    M68K_HALT_ILLEGAL,
    M68K_HALT_GUEST_EXIT, /* guest wrote the debug exit port */
};

/* The Motorola 68000 register file. PC and the An/Dn registers are 32-bit
 * (only the low 24 of PC ever address memory). a[7] is the *active* stack
 * pointer; the inactive one lives in usp/ssp and is swapped on an S-bit
 * change. Memory is big-endian — see mac_mem.h for the access helpers. */
typedef struct m68k_cpu {
    u32 d[8];
    u32 a[8];          /* a[7] = active SP */
    u32 pc;
    u16 sr;            /* status register; low byte is the CCR */
    u32 usp;           /* user stack pointer  (valid when in supervisor) */
    u32 ssp;           /* system stack pointer (valid when in user)      */

    u64 cycles;        /* free-running cycle counter */
    u64 instrs;        /* count of m68k_step calls — interp throughput stat */

    u8  stopped;       /* STOP instruction — waiting for an interrupt */
    u8  halted;        /* non-zero: run loop should exit (see enum above) */
    u8  exit_code;     /* value the guest passed to the debug exit port  */

    u8  pending_irq;   /* highest pending interrupt level (0 = none, 1..7) */

    /* Scratch slot used by a JIT block to stash its CALL0 return address
     * without modifying a1 (see jit/dispatcher.c — same trick as gbjit). */
    u32 jit_ret_pc;

    /* JIT-helper argument slots. Custom fast-path helpers (e.g. the MMIO
     * write path) read these instead of the (CALL0 ABI-conflicting) a3
     * register. The JIT bridge stores its computed addr/imm here before
     * the CALLX0. */
    u32 jit_arg1;
    u32 jit_arg2;

    /* JIT-chaining current-block pointer. The dispatcher sets this to
     * the active m68k_block* before invoking the block; the block's
     * epilogue (on ESP32 only) loads predicted_next from it and may
     * jx directly to the next block's entry without round-tripping the
     * dispatcher. Stored as void* to avoid m68k_block forward-decl
     * here — see codegen.h for the actual struct. */
    void *current_block;

    /* Periodic-dispatcher-return counter. Even with native chaining
     * the dispatcher must run mac_mem_tick / poll_interrupts. The block
     * epilogue decrements this; when it hits zero, fall back to a
     * dispatcher return regardless of chain prediction, and the
     * dispatcher refills it. */
    u32 chain_budget;

    /* JIT shared literal table. The session-invariant JIT literals (helper
     * pointers, RAM/ROM base+bounds, the helper-bridge addr — everything
     * except ADDR_CPU_BASE and the per-block branch PC) live here, loaded by
     * generated code with `l32i R_CPU, off` instead of a per-block `l32r`.
     * That removes the ~180-byte per-block literal pool: blocks shrink ~2x,
     * halving the L2-rehydrate PSRAM copy and ~doubling L1 residency. The
     * dispatcher refills it (cheap) so overlay-dependent bounds stay correct.
     * Indexed by literal_id (jit/codegen.h); sized with headroom. Kept within
     * 1020 bytes of the struct base so the l32i offset is in range. */
    u32 jit_lit[48];

    struct mac_mem *mem;
} m68k_cpu;

void m68k_reset(m68k_cpu *cpu, struct mac_mem *mem);

/* SR / CCR helpers. */
static inline u8 m68k_get_ccr(const m68k_cpu *cpu) { return (u8)(cpu->sr & 0x1Fu); }
static inline void m68k_set_ccr(m68k_cpu *cpu, u8 v) {
    cpu->sr = (u16)((cpu->sr & 0xFF00u) | (v & 0x1Fu));
}
static inline bool m68k_is_super(const m68k_cpu *cpu) { return (cpu->sr & SR_S) != 0; }

/* Swap the active SP with the inactive copy when the S bit changes. Call
 * AFTER updating cpu->sr's S bit. `was_super` is the S bit before. */
void m68k_sync_sp(m68k_cpu *cpu, bool was_super);

#endif
