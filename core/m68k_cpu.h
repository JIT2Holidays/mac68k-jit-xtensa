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

    /* 68010+ vector base register. Plus mode leaves this at 0 so exception
     * vectoring (mac_read32(mem, vector*4)) is bit-for-bit unchanged. The
     * SE/30 ROM moves the vector table out of low memory by writing VBR
     * via MOVEC. */
    u32 vbr;

    /* 68010+ source / destination function codes — modes the MOVES
     * instruction reads/writes through. We don't actually model function
     * codes in mac_mem (all accesses are equivalent); these fields just
     * give MOVEC something to write/read so the OS isn't surprised. */
    u32 sfc, dfc;

    /* 68020/030 cache control / cache address registers. Our emulation
     * has no on-chip cache; these are pure scratch the guest can write
     * and read back. */
    u32 cacr, caar;

    /* 68020+ master / interrupt stack pointers. The 68030 doesn't actually
     * implement the split-stack mode (that's 68020-only with the right
     * SR bit), but MOVEC to/from MSP/ISP must still round-trip values
     * cleanly. */
    u32 msp, isp;

    /* TODO(pmmu): register stub only — the SE/30's 68030 has an on-chip
     * PMMU. We accept PMOVE writes (so the SE/30 ROM doesn't fault) but
     * do NOT actually translate addresses. PFLUSH / PLOAD / PTEST are
     * no-ops. System 7 boots in 24-bit mode where the MMU is transparent;
     * 32-bit mode / Virtual Memory will need full PTW. */
    u64 srp, crp;          /* supervisor / CPU root pointers */
    u32 tc;                /* translation control register   */
    u32 tt0, tt1;          /* transparent translation 0 / 1  */
    u16 mmusr;             /* MMU status register            */

    /* Deferred-bus-error indicator. When mac_read or mac_write on SE/30
     * touches an unmapped address it sets this to the faulting address
     * (with the high bit OR'd in for non-zero); the run loop and
     * m68k_step pick it up after the current instruction and raise
     * vector 2. The SE/30 ROM uses BERR as a hardware-probe mechanism:
     * it sets bit 27 of D7 + a recovery handler in A6, then accesses
     * memory; on BERR the handler at vector 2 jumps via A6. Without
     * this the ROM cannot detect which chips are present and loops
     * forever waiting on signals from un-modeled hardware. */
    u32 bus_error_pending;
/* M7.6l — bus_error_pending encoding. Bit 31 is the "pending" flag the
 * run loop tests. Bits 30-28 are the cause; PTEST decodes these into
 * MMUSR. Bits 27-0 hold the faulting logical address (low 28 bits, but
 * in practice we never set BERR with LAs above 0x0FFFFFFF in current
 * code paths). */
#define BERR_CAUSE_SHIFT     28
#define BERR_CAUSE_MASK      (7u << BERR_CAUSE_SHIFT)
#define BERR_CAUSE_GENERIC   (0u << BERR_CAUSE_SHIFT)
#define BERR_CAUSE_WP        (1u << BERR_CAUSE_SHIFT)
#define BERR_CAUSE_SUPER     (2u << BERR_CAUSE_SHIFT)
#define BERR_CAUSE_INVALID   (3u << BERR_CAUSE_SHIFT)
#define BERR_CAUSE_OOR       (4u << BERR_CAUSE_SHIFT)

    /* M7.6r — captured faulting address for the 030 format-A BERR stack
     * frame. The run loop pulls the LA out of bus_error_pending and
     * stores it here before calling m68k_exception(cpu, 2). The frame
     * builder copies this to SP+0x0E so the ROM's BERR handler can read
     * the LA it asked for. */
    u32 fault_addr;

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
