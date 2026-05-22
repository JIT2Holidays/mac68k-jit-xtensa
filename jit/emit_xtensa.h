#ifndef EMIT_XTENSA_H
#define EMIT_XTENSA_H

#include "m68k_types.h"

/* Xtensa LX7 instruction encoder. Generates host-endian little-endian byte
   streams matching the Xtensa core ISA + density option (16-bit narrow forms).
   All encode_* functions return the number of bytes written. Buffers are
   assumed to have at least 3 bytes available. */

typedef struct {
    u8  *buf;
    u32  len;
    u32  cap;
    /* Set true the first time an emit would exceed `cap`. The emit is
     * dropped (nothing written past the buffer) and every later emit is
     * also dropped. gbjit_compile_block checks this after emission and
     * discards the whole block — a too-fat block becomes a clean interp
     * fallback instead of a silent write past the codecache arena.
     * (In debug builds the bounds assert still fires first.) */
    bool overflow;
    /* Word-packing accumulator: holds the bytes for the in-progress
     * 4-byte word. Flushed to buf via a 32-bit store every time len
     * crosses a 4-byte boundary, and on xt_flush_pending() at the end
     * of emission. This lets us avoid byte stores to executable
     * memory, which fault with LoadStoreError on ESP32-S3 IRAM (and
     * on plain ESP32 IRAM without the trap handler). */
    u32  word_acc;
} xt_emit;

void xt_init(xt_emit *e, u8 *buf, u32 cap);
/* Flush any partial-word accumulator at the tail. Must be called once
 * before any final use of `buf` (execution, patching across words, etc.). */
void xt_flush_pending(xt_emit *e);

/* --- Core instruction encoders ---
   ar = destination register, as = source, at = source/target index.
   imm = immediate. Out-of-range immediates trip an assertion in debug. */

/* Move/load-immediate. */
u32 xt_movi(xt_emit *e, u8 at, i32 imm);            /* MOVI at, imm (-2048..2047) */
u32 xt_mov(xt_emit *e, u8 ar, u8 as);               /* MOV ar, as (= OR ar, as, as) */
u32 xt_l32r(xt_emit *e, u8 at, u32 lit_offset);     /* L32R at, label */

/* Arithmetic / logical (3-operand). */
u32 xt_add(xt_emit *e, u8 ar, u8 as, u8 at);
u32 xt_sub(xt_emit *e, u8 ar, u8 as, u8 at);
u32 xt_addi(xt_emit *e, u8 at, u8 as, i32 imm);     /* ADDI at, as, imm (-128..127) */
u32 xt_and(xt_emit *e, u8 ar, u8 as, u8 at);
u32 xt_or (xt_emit *e, u8 ar, u8 as, u8 at);
u32 xt_xor(xt_emit *e, u8 ar, u8 as, u8 at);

/* Shifts (immediate). LX7 has SLLI/SRLI/SRAI. */
u32 xt_slli(xt_emit *e, u8 ar, u8 as, u8 sa);       /* sa = 1..31 */
u32 xt_srli(xt_emit *e, u8 ar, u8 as, u8 sa);       /* sa = 0..15  */
u32 xt_srai(xt_emit *e, u8 ar, u8 as, u8 sa);       /* sa = 0..31  */

/* Extract / mask helpers. */
u32 xt_extui(xt_emit *e, u8 ar, u8 at, u8 shiftimm, u8 maskimm); /* maskimm=width-1 logical */

/* Memory (8-bit load/store, zero-extend for L8UI). */
u32 xt_l8ui (xt_emit *e, u8 at, u8 as, u32 off);    /* off 0..255 */
u32 xt_s8i  (xt_emit *e, u8 at, u8 as, u32 off);
u32 xt_l16ui(xt_emit *e, u8 at, u8 as, u32 off);    /* off 0..510 step 2 */
u32 xt_s16i (xt_emit *e, u8 at, u8 as, u32 off);
u32 xt_l32i (xt_emit *e, u8 at, u8 as, u32 off);    /* off 0..1020 step 4 */
u32 xt_s32i (xt_emit *e, u8 at, u8 as, u32 off);

/* Branches and jumps. */
u32 xt_beqi (xt_emit *e, u8 as, i32 imm, i32 rel);  /* tiny encoded subset */
u32 xt_bnei (xt_emit *e, u8 as, i32 imm, i32 rel);
u32 xt_beqz (xt_emit *e, u8 as, i32 rel);
u32 xt_bnez (xt_emit *e, u8 as, i32 rel);
u32 xt_j    (xt_emit *e, i32 rel);                  /* unconditional, +/-128KB */
u32 xt_jx   (xt_emit *e, u8 as);                    /* indirect via reg */

/* CALL0/RET. */
u32 xt_call0 (xt_emit *e, i32 rel);                 /* aligned target offset */
u32 xt_callx0(xt_emit *e, u8 as);
u32 xt_ret   (xt_emit *e);                          /* RET = JX a0 */

/* Raw word emit (literal pool / pad). */
u32 xt_raw32(xt_emit *e, u32 word);

#endif
