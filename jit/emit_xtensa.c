/* Xtensa LX7 instruction encoder.
 *
 * 24-bit instructions are stored little-endian as 3 bytes. Bit positions in
 * the comments refer to the 24-bit instruction word with bit 0 = LSB.
 *
 * Encodings sourced from the canonical Xtensa ISA reference (op0/op1/op2/r/s/t
 * field layout). See PLAN.md §2 and the table at the top of this file.
 *
 * Field map for 24-bit instructions:
 *   bits  0.. 3 : op0      (instruction class)
 *   bits  4.. 7 : t        (target/operand)
 *   bits  8..11 : s        (source)
 *   bits 12..15 : r        (destination for RRR; selector for RRI8)
 *   bits 16..19 : op1
 *   bits 20..23 : op2
 *
 * Formats used here:
 *   RRR   : op0=0; op2 selects op; uses r,s,t as register fields
 *   RRI8  : op0=2; r selects op; uses imm8 at bits 16..23
 *   RRI4  : op0=1 (L32R) etc., immediate at bits 8..23
 *   CALL  : op0=5 (CALL0); 18-bit offset at bits 6..23
 *   BR    : op0=6 (J,BEQZ,BNEZ); offset at bits 6..23 (J) or 12..23 (Bxxx)
 */

#include "emit_xtensa.h"
#include <assert.h>
#include <string.h>

void xt_init(xt_emit *e, u8 *buf, u32 cap) {
    e->buf = buf;
    e->len = 0;
    e->cap = cap;
    e->word_acc = 0;
    e->overflow = false;
}

void xt_flush_pending(xt_emit *e) {
    /* Flush the current partial word (if any) so its bytes become visible
     * in buf. Unwritten byte positions remain 0 (word_acc was init'd to
     * 0); that matches the memset() the codecache does after alloc. */
    if (e->len & 3u) {
        u32 word_off = e->len & ~3u;
        *(u32 *)(e->buf + word_off) = e->word_acc;
    }
}

static inline void emit_byte_packed(xt_emit *e, u8 b) {
    /* Pack `b` into the current word at the right byte slot. When that
     * makes the word complete, flush it to buf via a 32-bit store —
     * never touch the buffer with an 8-bit access, which faults on
     * IRAM-resident exec memory on ESP32-S3 (and on plain ESP32 IRAM
     * without the LoadStoreError trap handler). */
    u32 byte_in_word = e->len & 3u;
    e->word_acc |= ((u32)b) << (byte_in_word * 8);
    e->len++;
    if ((e->len & 3u) == 0) {
        *(u32 *)(e->buf + (e->len - 4)) = e->word_acc;
        e->word_acc = 0;
    }
}

static inline u32 emit24(xt_emit *e, u32 w) {
    assert(e->len + 3 <= e->cap);
    /* Release builds have no assert: a real bounds check keeps a
     * budget-busting block from scribbling past the codecache arena. */
    if (e->len + 3 > e->cap) { e->overflow = true; return 0; }
    emit_byte_packed(e, (u8)(w & 0xFFu));
    emit_byte_packed(e, (u8)((w >> 8) & 0xFFu));
    emit_byte_packed(e, (u8)((w >> 16) & 0xFFu));
    return 3;
}

/* RRR-format helper. op0=0 always for the basic arithmetic/logical ops here.
 *   bits 0..3   = 0  (op0)
 *   bits 4..7   = t
 *   bits 8..11  = s
 *   bits 12..15 = r
 *   bits 16..19 = op1
 *   bits 20..23 = op2
 */
static inline u32 enc_rrr(u8 op2, u8 op1, u8 r, u8 s, u8 t) {
    return  ((u32)(op2 & 0xF) << 20)
          | ((u32)(op1 & 0xF) << 16)
          | ((u32)(r   & 0xF) << 12)
          | ((u32)(s   & 0xF) <<  8)
          | ((u32)(t   & 0xF) <<  4)
          | 0x0;
}

/* RRI8-format helper. op0=2 for the load/store/ADDI/MOVI family.
 *   bits 0..3   = 2  (op0)
 *   bits 4..7   = t
 *   bits 8..11  = s
 *   bits 12..15 = r  (op subclass selector)
 *   bits 16..23 = imm8
 */
static inline u32 enc_rri8(u8 r, u8 s, u8 t, u8 imm8) {
    return  ((u32)imm8 << 16)
          | ((u32)(r & 0xF) << 12)
          | ((u32)(s & 0xF) <<  8)
          | ((u32)(t & 0xF) <<  4)
          | 0x2;
}

/* --- RRR arithmetic / logical. op0=0, op1=0; op2 selects op. --- */
/*  AND  op2=0x1     OR   op2=0x2     XOR  op2=0x3
 *  ADD  op2=0x8     SUB  op2=0xC                              */
u32 xt_and(xt_emit *e, u8 ar, u8 as, u8 at) { return emit24(e, enc_rrr(0x1, 0, ar, as, at)); }
u32 xt_or (xt_emit *e, u8 ar, u8 as, u8 at) { return emit24(e, enc_rrr(0x2, 0, ar, as, at)); }
u32 xt_xor(xt_emit *e, u8 ar, u8 as, u8 at) { return emit24(e, enc_rrr(0x3, 0, ar, as, at)); }
u32 xt_add(xt_emit *e, u8 ar, u8 as, u8 at) { return emit24(e, enc_rrr(0x8, 0, ar, as, at)); }
u32 xt_sub(xt_emit *e, u8 ar, u8 as, u8 at) { return emit24(e, enc_rrr(0xC, 0, ar, as, at)); }
u32 xt_mov(xt_emit *e, u8 ar, u8 as) { return xt_or(e, ar, as, as); }

/* --- RRI8 family. op0=2; r selects op. --- */
/*  L8UI  r=0   S8I  r=4   L16UI r=1   S16I r=5
 *  L32I  r=2   S32I r=6   ADDI  r=C   MOVI r=A                */
u32 xt_addi(xt_emit *e, u8 at, u8 as, i32 imm) {
    assert(imm >= -128 && imm <= 127);
    return emit24(e, enc_rri8(0xC, as, at, (u8)imm));
}

/* MOVI uses a 12-bit signed immediate split as: low 8 bits in imm8 (bits 16..23),
 * high 4 bits in s (bits 8..11). Range: -2048..+2047. */
u32 xt_movi(xt_emit *e, u8 at, i32 imm) {
    assert(imm >= -2048 && imm <= 2047);
    u32 v = (u32)imm & 0xFFFu;
    return emit24(e, enc_rri8(0xA, (u8)((v >> 8) & 0xF), at, (u8)(v & 0xFF)));
}

u32 xt_l8ui(xt_emit *e, u8 at, u8 as, u32 off) {
    assert(off <= 255);
    return emit24(e, enc_rri8(0x0, as, at, (u8)off));
}
u32 xt_s8i(xt_emit *e, u8 at, u8 as, u32 off) {
    assert(off <= 255);
    return emit24(e, enc_rri8(0x4, as, at, (u8)off));
}
u32 xt_l16ui(xt_emit *e, u8 at, u8 as, u32 off) {
    assert(off <= 510 && (off & 1) == 0);
    return emit24(e, enc_rri8(0x1, as, at, (u8)(off >> 1)));
}
u32 xt_s16i(xt_emit *e, u8 at, u8 as, u32 off) {
    assert(off <= 510 && (off & 1) == 0);
    return emit24(e, enc_rri8(0x5, as, at, (u8)(off >> 1)));
}
u32 xt_l32i(xt_emit *e, u8 at, u8 as, u32 off) {
    assert(off <= 1020 && (off & 3) == 0);
    return emit24(e, enc_rri8(0x2, as, at, (u8)(off >> 2)));
}
u32 xt_s32i(xt_emit *e, u8 at, u8 as, u32 off) {
    assert(off <= 1020 && (off & 3) == 0);
    return emit24(e, enc_rri8(0x6, as, at, (u8)(off >> 2)));
}

/* --- L32R: PC-relative load of a 32-bit literal (the literal lives BEFORE
 *           the current instruction at a 4-aligned address, within -262144..0).
 *   op0=1, t=at, imm16 at bits 8..23.
 *   imm16 = (literal_addr - (PC & ~3) - 3) >> 2  ... encoded as 16-bit.
 *   Caller computes the right imm16; we encode raw.                          */
u32 xt_l32r(xt_emit *e, u8 at, u32 lit_offset) {
    u16 imm16 = (u16)lit_offset;
    return emit24(e, ((u32)imm16 << 8) | ((u32)(at & 0xF) << 4) | 0x1);
}

/* --- Branches: BEQZ / BNEZ.
 *   op0 = 6.
 *   Field at bits 4..7 distinguishes:  BEQZ -> 0x1   BNEZ -> 0x5
 *   s field (bits 8..11) holds source register.
 *   imm12 at bits 12..23 = signed PC-relative offset, where target = PC + 4 + imm12. */
static inline u32 enc_bxxz(u8 sel_t, u8 as, i32 rel) {
    i32 off = rel - 4;
    assert(off >= -2048 && off <= 2047);
    u32 imm12 = (u32)off & 0xFFFu;
    return ((u32)imm12 << 12)
         | ((u32)(as & 0xF) << 8)
         | ((u32)(sel_t & 0xF) << 4)
         | 0x6;
}
u32 xt_beqz(xt_emit *e, u8 as, i32 rel) { return emit24(e, enc_bxxz(0x1, as, rel)); }
u32 xt_bnez(xt_emit *e, u8 as, i32 rel) { return emit24(e, enc_bxxz(0x5, as, rel)); }

/* BEQI/BNEI — RRI8 sub-family but with different op0. Not used yet. */
u32 xt_beqi(xt_emit *e, u8 as, i32 imm, i32 rel) {
    (void)e; (void)as; (void)imm; (void)rel;
    assert(0 && "BEQI not yet implemented");
    return 0;
}
u32 xt_bnei(xt_emit *e, u8 as, i32 imm, i32 rel) {
    (void)e; (void)as; (void)imm; (void)rel;
    assert(0 && "BNEI not yet implemented");
    return 0;
}

/* --- Unconditional jumps and calls.
 *   J:      op0=6, n=2  (bits 4..5 = 10). Offset 18-bit at bits 6..23.
 *   CALL0:  op0=5, n=0. Offset 18-bit at bits 6..23, target = (PC & ~3) + 4 + (offset<<2).
 *   JX:     RRR with op0=0, t=0xA, s=as.  (0x0000A0 | (as<<8))
 *   CALLX0: RRR with op0=0, t=0xC, s=as.  (0x0000C0 | (as<<8))
 *   RET:    a fixed encoding 0x000080 in CALL0 ABI.                          */
u32 xt_j(xt_emit *e, i32 rel) {
    /* Verified against xtensa-esp32s3-elf-as: bits 0..5 = 0x06, bits 6..23 =
     * 18-bit signed offset, target = pc + 4 + offset. */
    i32 off = rel - 4;
    assert(off >= -(1 << 17) && off < (1 << 17));
    u32 imm18 = (u32)off & 0x3FFFFu;
    return emit24(e, (imm18 << 6) | 0x06u);
}
u32 xt_call0(xt_emit *e, i32 rel) {
    /* CALL0: bits 0..5 = 0x05 (op0=5, n=0). Target must be 4-aligned. */
    assert((rel & 3) == 0);
    i32 off = rel - 4;
    assert((off & 3) == 0);
    off >>= 2;
    assert(off >= -(1 << 17) && off < (1 << 17));
    u32 imm18 = (u32)off & 0x3FFFFu;
    return emit24(e, (imm18 << 6) | 0x05u);
}
u32 xt_jx(xt_emit *e, u8 as) {
    /* JX as: op0=0, op1=0, op2=0, r=0, s=as, t=0xA. */
    return emit24(e, ((u32)(as & 0xF) << 8) | (0xAu << 4) | 0x0);
}
u32 xt_callx0(xt_emit *e, u8 as) {
    /* CALLX0 as: t=0xC. */
    return emit24(e, ((u32)(as & 0xF) << 8) | (0xCu << 4) | 0x0);
}
u32 xt_ret(xt_emit *e) {
    /* RET (CALL0 ABI): fixed encoding 0x000080. */
    return emit24(e, 0x000080u);
}

/* --- Shifts.
 *
 *   SLLI ar, as, sa  (sa in 1..31)
 *     RRR-like: op0=0, op2 = 0xA | (sa>>4)<<3 ... Xtensa stores SLLI's sa
 *     unusually as (32 - sa) split across bits 4..7 (low 4) and bit 20 of op2.
 *     Encoding (deduced from ida tables, op2=0x1|sa_hi<<3, op1=0x1):
 *       sa1 = 32 - sa
 *       op2 = 0x1 | ((sa1 >> 4) << 3)  ∈ {0x1, 0x9}
 *       op1 = 0x1
 *       r   = ar
 *       s   = as
 *       t   = sa1 & 0xF
 *
 *   SRLI ar, at, sa  (sa in 0..15)
 *     op0=0, op2=0x4, op1=0x1
 *     r = ar,  s = sa (0..15),  t = at
 *
 *   SRAI ar, at, sa  (sa in 0..31)
 *     op0=0, op2 = 0x2 | ((sa>>4)<<3) ∈ {0x2, 0xA}
 *     op1=0x1
 *     r = ar,  s = sa & 0xF,  t = at
 */
u32 xt_slli(xt_emit *e, u8 ar, u8 as, u8 sa) {
    /* Canonical: bits 21..23 = 0 (fixed). sa_hi1 lives at bit 20 (op2 LSB). */
    assert(sa >= 1 && sa <= 31);
    u8 sa1 = (u8)(32u - sa);
    u8 op2 = (u8)((sa1 >> 4) & 1);
    return emit24(e, enc_rrr(op2, 0x1, ar, as, (u8)(sa1 & 0xF)));
}
u32 xt_srli(xt_emit *e, u8 ar, u8 as, u8 sa) {
    assert(sa <= 15);
    return emit24(e, enc_rrr(0x4, 0x1, ar, sa, as));
}
u32 xt_srai(xt_emit *e, u8 ar, u8 as, u8 sa) {
    /* Canonical: bit 21 fixed = 1, bits 22..23 = 0. So op2 = 2 | sa_hi1. */
    assert(sa <= 31);
    u8 op2 = (u8)(0x2 | ((sa >> 4) & 1));
    return emit24(e, enc_rrr(op2, 0x1, ar, (u8)(sa & 0xF), as));
}

/* EXTUI ar, at, shiftimm (0..31), maskimm (0..15 = width-1).
 *   Canonical layout (verified against xtensa-esp32s3-elf-as):
 *     bits  0..3  = 0  (op0)
 *     bits  4..7  = t (= at, source)
 *     bits  8..11 = sh_lo4
 *     bits 12..15 = r (= ar, dest)
 *     bit  16     = sh_hi1
 *     bit  17     = 0  (fixed)
 *     bit  18     = 1  (fixed)
 *     bit  19     = 0  (fixed)
 *     bits 20..23 = maskimm
 *   So in RRR-field terms: op0=0, op1 = 0x4 | sh_hi1, op2 = maskimm.
 */
u32 xt_extui(xt_emit *e, u8 ar, u8 at, u8 shiftimm, u8 maskimm) {
    assert(shiftimm <= 31);
    assert(maskimm <= 15);
    u8 sh_lo4 = (u8)(shiftimm & 0xF);
    u8 sh_hi1 = (u8)((shiftimm >> 4) & 1);
    return emit24(e, enc_rrr((u8)(maskimm & 0xF), (u8)(0x4 | sh_hi1), ar, sh_lo4, at));
}

u32 xt_raw32(xt_emit *e, u32 word) {
    assert(e->len + 4 <= e->cap);
    if (e->len + 4 > e->cap) { e->overflow = true; return 0; }
    e->buf[e->len + 0] = (u8)(word & 0xFF);
    e->buf[e->len + 1] = (u8)((word >> 8) & 0xFF);
    e->buf[e->len + 2] = (u8)((word >> 16) & 0xFF);
    e->buf[e->len + 3] = (u8)((word >> 24) & 0xFF);
    e->len += 4;
    return 4;
}
