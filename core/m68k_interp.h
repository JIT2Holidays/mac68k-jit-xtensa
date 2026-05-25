#ifndef M68K_INTERP_H
#define M68K_INTERP_H

#include "m68k_types.h"
#include "m68k_cpu.h"

/* Reference Motorola 68000 interpreter.
 *
 * This is the correctness oracle: the JIT's differential test runs guest
 * code under both this interpreter and the JIT and diffs the register
 * file. It is also the JIT's fallback — gbjit-style, the JIT emits a
 * CALLX0 to m68k_step for any instruction it does not translate inline. */

/* Execute exactly one instruction at cpu->pc. Advances pc and cycles.
 * Does NOT poll for interrupts — the run loop / dispatcher owns that so a
 * JIT block can call this per-instruction without surprise control flow. */
void m68k_step(m68k_cpu *cpu);

/* JIT custom fast-path helpers — see definitions in m68k_interp.c. */
void m68k_jit_ori_b_mmio(m68k_cpu *cpu);
void m68k_jit_btst_b_mmio(m68k_cpu *cpu);
void m68k_jit_movem_l_postinc_to_regs(m68k_cpu *cpu);
void m68k_jit_movem_l_predec_from_regs(m68k_cpu *cpu);
void m68k_jit_movem_w_to_mem(m68k_cpu *cpu);
void m68k_jit_movem_l_to_mem(m68k_cpu *cpu);
void m68k_jit_move_w_postinc_to_dn(m68k_cpu *cpu);
void m68k_jit_rts_mmio(m68k_cpu *cpu);
void m68k_jit_bsr_s_mmio(m68k_cpu *cpu);
void m68k_jit_bsr_w_mmio(m68k_cpu *cpu);
void m68k_jit_move_l_postinc_to_dn_mmio(m68k_cpu *cpu);
void m68k_jit_move_b_addr_to_dn_mmio(m68k_cpu *cpu);
void m68k_jit_move_b_dn_to_addr_mmio(m68k_cpu *cpu);
void m68k_jit_move_l_dn_to_anpi_mmio(m68k_cpu *cpu);
void m68k_jit_move_b_addr_to_an_mmio(m68k_cpu *cpu);
void m68k_jit_move_b_imm_to_addr_mmio(m68k_cpu *cpu);
void m68k_jit_fline_trap(m68k_cpu *cpu);
void m68k_jit_move_l_an_to_dn_mmio(m68k_cpu *cpu);
void m68k_jit_clr_w_anpi_mmio(m68k_cpu *cpu);
void m68k_jit_tst_b_mmio(m68k_cpu *cpu);
void m68k_jit_aline_trap(m68k_cpu *cpu);
void m68k_jit_move_anpi_to_sr(m68k_cpu *cpu);

/* Run the pure interpreter until cpu->cycles >= until or the CPU halts.
 * Polls interrupts and ticks the peripherals between instructions. */
void m68k_run_until(m68k_cpu *cpu, u64 until);

/* Service a pending interrupt if its level outranks the SR mask. Returns
 * true if an interrupt was taken. The dispatcher calls this between
 * blocks; m68k_run_until calls it between instructions. */
bool m68k_poll_interrupts(m68k_cpu *cpu);

/* Raise a CPU exception: push the frame and vector through (vector*4). */
void m68k_exception(m68k_cpu *cpu, u32 vector);

/* Decode just enough of the instruction at `pc` to drive the JIT's basic-
 * block discovery: total length in bytes, and whether it ends a block
 * (any branch / jump / return / trap / stop). */
typedef struct m68k_decoded {
    u32  length;       /* instruction size in bytes (>=2) */
    bool ends_block;   /* control-flow instruction — terminates the block */
    u16  opcode;       /* the first instruction word */
} m68k_decoded;

m68k_decoded m68k_decode_at(m68k_cpu *cpu, u32 pc);

/* CPU exception ring log {vector, faulting pc, cycle} and a debug hook
 * invoked on every line-A (Toolbox) trap. */
extern u32 m68k_exc_log[64][3];
extern u32 m68k_exc_n;
extern void (*m68k_trap_hook)(m68k_cpu *cpu, u16 trap);

#endif
