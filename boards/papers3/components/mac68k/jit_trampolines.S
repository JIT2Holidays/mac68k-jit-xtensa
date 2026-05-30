/* CALL0 -> windowed ABI bridge for the JIT.
 *
 * A compiled block runs under the CALL0 ABI (a0 = return address, no
 * register-window rotation). The reference interpreter m68k_step it falls
 * back to is an ordinary windowed-ABI C function. This shim crosses that
 * boundary.
 *
 * Crucially it does NOT modify a1. Modifying a1 from a CALL0 function that
 * executes inside a windowed parent's stack frame would shift the parent's
 * register-spill target and corrupt memory on a window overflow. The
 * caller's a0 lives in the phys-register range that CALL8 preserves, so it
 * survives the windowed call automatically.
 *
 * Copied verbatim from port/esp32s3 — board-agnostic LX7 glue. */

    .text

    .global m68k_step_call0
    .type   m68k_step_call0, @function
    .align  4
m68k_step_call0:
    mov     a10, a2          /* cpu pointer -> callee's a2 after CALL8 */
    call8   m68k_step
    jx      a0               /* return to the JIT block */
    .size   m68k_step_call0, .-m68k_step_call0
