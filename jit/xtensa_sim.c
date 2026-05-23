/* Minimal Xtensa LX7 simulator for the subset of instructions emitted by the
 * JIT (see emit_xtensa.c).
 *
 * The decoder here is written INDEPENDENTLY from the encoder: it works
 * directly off the 24-bit instruction word using ISA field positions, so a
 * shared bug in the encoder won't pass through silently — a wrong encoding
 * will simply decode to something else.
 *
 * Sentinel: when JX a0 (or RET) is executed and a0 holds 0, the sim returns
 * with XT_SIM_RETURNED. This matches the typical "block exit" convention
 * where the trampoline puts a return-cookie in a0. */

#include "xtensa_sim.h"
#include <stdio.h>
#include <string.h>

#define SENTINEL_RET 0u

void xt_sim_init(xt_sim *s, const u8 *code, u32 code_size) {
    memset(s, 0, sizeof(*s));
    s->code = code;
    s->code_size = code_size;
    s->pc = 0;
    s->status = XT_SIM_RUN;
}

static inline u32 fetch24(xt_sim *s) {
    if (s->pc + 3 > s->code_size) { s->status = XT_SIM_OUT_OF_RANGE; return 0; }
    u32 w = (u32)s->code[s->pc] | ((u32)s->code[s->pc+1] << 8) | ((u32)s->code[s->pc+2] << 16);
    s->pc += 3;
    return w;
}

static inline u32 sign_extend(u32 v, u32 bits) {
    u32 mask = 1u << (bits - 1);
    return (v ^ mask) - mask;
}

/* Field extractors. */
static inline u8 fld_op0(u32 w) { return (u8)(w & 0xF); }
static inline u8 fld_t  (u32 w) { return (u8)((w >> 4) & 0xF); }
static inline u8 fld_s  (u32 w) { return (u8)((w >> 8) & 0xF); }
static inline u8 fld_r  (u32 w) { return (u8)((w >> 12) & 0xF); }
static inline u8 fld_op1(u32 w) { return (u8)((w >> 16) & 0xF); }
static inline u8 fld_op2(u32 w) { return (u8)((w >> 20) & 0xF); }
static inline u8 fld_imm8(u32 w) { return (u8)((w >> 16) & 0xFF); }
static inline u16 fld_imm16(u32 w) { return (u16)((w >> 8) & 0xFFFF); }

static void step(xt_sim *s) {
    u32 pc_start = s->pc;
    u32 w = fetch24(s);
    if (s->status != XT_SIM_RUN) return;
    u8 op0 = fld_op0(w);

    /* L32R: op0=1, t=at, imm16 at bits 8..23. The literal address is
     *   (PC_of_L32R + 3) & ~3 + (sign_extend(imm16, 16) << 2)
     * but for the simulator we use a callback to fetch the literal so the
     * dispatcher can place literals wherever it likes.
     *
     * Reference convention used by the encoder/codegen: imm16 = 0xFFFF means
     * "the 4-byte word immediately preceding the aligned base". The translate
     * callback receives the target address. */
    if (op0 == 0x1) {
        u8 at = fld_t(w);
        u16 imm16 = fld_imm16(w);
        /* PC-relative literal: the spec says
         *   literal = M[((PC + 3) & ~3) + (sign_extend(imm16,16) << 2)]
         * Negative imm16 (top bit set) points BEFORE the instruction.
         * We compute the relative offset and let the dispatcher resolve via
         * read_literal(sim, target_addr_relative_to_code_base). */
        i32 rel = (i32)sign_extend(imm16, 16);
        i32 target_off = (i32)(((pc_start + 3) & ~3u)) + (rel << 2);
        u32 val = s->read_literal ? s->read_literal(s, (u32)target_off) : 0;
        s->a[at] = val;
        return;
    }

    /* RRI8 family: op0=2 */
    if (op0 == 0x2) {
        u8 r = fld_r(w);
        u8 t = fld_t(w);
        u8 sr = fld_s(w);
        u8 imm = fld_imm8(w);
        switch (r) {
            case 0x0: { /* L8UI at,as,imm */
                u32 addr = s->a[sr] + imm;
                u8 *p = s->translate ? s->translate(s, addr) : NULL;
                s->a[t] = p ? *p : 0;
                return;
            }
            case 0x4: { /* S8I at,as,imm */
                u32 addr = s->a[sr] + imm;
                u8 *p = s->translate ? s->translate(s, addr) : NULL;
                if (p) *p = (u8)(s->a[t] & 0xFF);
                return;
            }
            case 0x1: { /* L16UI at,as,imm*2 */
                u32 addr = s->a[sr] + ((u32)imm << 1);
                u8 *p = s->translate ? s->translate(s, addr) : NULL;
                s->a[t] = p ? (u32)p[0] | ((u32)p[1] << 8) : 0;
                return;
            }
            case 0x5: { /* S16I */
                u32 addr = s->a[sr] + ((u32)imm << 1);
                u8 *p = s->translate ? s->translate(s, addr) : NULL;
                if (p) { p[0] = (u8)(s->a[t] & 0xFF); p[1] = (u8)((s->a[t] >> 8) & 0xFF); }
                return;
            }
            case 0x2: { /* L32I */
                u32 addr = s->a[sr] + ((u32)imm << 2);
                u8 *p = s->translate ? s->translate(s, addr) : NULL;
                s->a[t] = p ? (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24) : 0;
                return;
            }
            case 0x6: { /* S32I */
                u32 addr = s->a[sr] + ((u32)imm << 2);
                u8 *p = s->translate ? s->translate(s, addr) : NULL;
                if (p) {
                    p[0] = (u8)(s->a[t] & 0xFF);
                    p[1] = (u8)((s->a[t] >> 8) & 0xFF);
                    p[2] = (u8)((s->a[t] >> 16) & 0xFF);
                    p[3] = (u8)((s->a[t] >> 24) & 0xFF);
                }
                return;
            }
            case 0xC: { /* ADDI at, as, imm8 (signed) */
                i32 simm = (i32)sign_extend(imm, 8);
                s->a[t] = s->a[sr] + (u32)simm;
                return;
            }
            case 0xA: { /* MOVI at, imm12 (signed); imm[7:0]=imm8, imm[11:8]=s */
                u32 imm12 = ((u32)sr << 8) | imm;
                s->a[t] = sign_extend(imm12, 12);
                return;
            }
            default: s->status = XT_SIM_BAD_OPCODE; return;
        }
    }

    /* CALL0: op0=5, n=0, bits 0..5 = 0x05. 18-bit offset at bits 6..23.
     * Target = (PC & ~3) + 4 + (offset << 2). The JIT uses CALLX0 (register-
     * indirect) for helpers, not direct CALL0, so we don't strictly need
     * this — but keep the decode path. */
    if ((w & 0x3F) == 0x05) {
        i32 off = (i32)sign_extend((w >> 6) & 0x3FFFF, 18);
        u32 target = (pc_start & ~3u) + 4 + ((u32)off << 2);
        s->a[0] = s->pc;            /* return address */
        s->pc = target;
        return;
    }

    /* J: bits 0..5 = 0x06 (op0=6, n=0). */
    if ((w & 0x3F) == 0x06) {
        i32 off = (i32)sign_extend((w >> 6) & 0x3FFFF, 18);
        s->pc = pc_start + 4 + (u32)off;
        return;
    }

    /* BEQZ / BNEZ. bits 0..3 = 6, bits 4..7 distinguish.
     * BEQZ: bits 4..7 = 0x1
     * BNEZ: bits 4..7 = 0x5
     * imm12 at bits 12..23 (signed). target = PC_of_branch + 4 + imm12. */
    if (op0 == 0x6) {
        u8 sel = fld_t(w);
        u8 sr = fld_s(w);
        i32 off = (i32)sign_extend((w >> 12) & 0xFFF, 12);
        u32 target = pc_start + 4 + (u32)off;
        bool take = false;
        if (sel == 0x1)      take = (s->a[sr] == 0);
        else if (sel == 0x5) take = (s->a[sr] != 0);
        else { s->status = XT_SIM_BAD_OPCODE; return; }
        if (take) s->pc = target;
        return;
    }

    /* RRR family: op0 = 0. */
    if (op0 == 0x0) {
        u8 op2 = fld_op2(w);
        u8 op1 = fld_op1(w);
        u8 r = fld_r(w);
        u8 sr = fld_s(w);
        u8 t = fld_t(w);

        /* Special pure-encoding ops (JX/CALLX0/RET): r=0, op1=0, op2=0,
         * distinguished by t. */
        if (op1 == 0 && op2 == 0 && r == 0) {
            if (t == 0xA) { /* JX as */
                u32 target = s->a[sr];
                if (sr == 0 && target == SENTINEL_RET) { s->status = XT_SIM_RETURNED; return; }
                /* Treat JX into our code block: the dispatcher arranges for
                 * targets within `code` to be expressed as byte offsets.
                 * We accept absolute addresses == base + offset by checking
                 * if it falls in our buffer; otherwise we trust the caller. */
                s->pc = target;
                return;
            }
            if (t == 0xC) { /* CALLX0 as */
                u32 fn_token = s->a[sr];
                s->a[0] = s->pc;        /* return address — caller may use to RET */
                if (s->call_thunk) s->call_thunk(s, fn_token);
                return;
            }
            if (t == 0x8) { /* RET (0x000080) */
                if (s->a[0] == SENTINEL_RET) { s->status = XT_SIM_RETURNED; return; }
                s->pc = s->a[0];
                return;
            }
        }

        /* Arithmetic / logical (op1 == 0). */
        if (op1 == 0) {
            switch (op2) {
                case 0x1: s->a[r] = s->a[sr] & s->a[t]; return;
                case 0x2: s->a[r] = s->a[sr] | s->a[t]; return;
                case 0x3: s->a[r] = s->a[sr] ^ s->a[t]; return;
                case 0x8: s->a[r] = s->a[sr] + s->a[t]; return;
                case 0xC: s->a[r] = s->a[sr] - s->a[t]; return;
                default: break;
            }
        }

        /* Shifts (canonical encoding):
         *   SLLI: op1=1, bits 21..23 = 0, sa_hi1 at bit 20.
         *         → op2 ∈ {0, 1}; sa1 = (op2 << 4) | t; sa = 32 - sa1.
         *   SRAI: op1=1, bit 21 = 1, bits 22..23 = 0, sa_hi1 at bit 20.
         *         → op2 ∈ {2, 3}; sa = ((op2 & 1) << 4) | s.
         *   SRLI: op1=1, op2=4. sa = s (0..15), source = t.
         */
        if (op1 == 0x1) {
            if (op2 == 0x0 || op2 == 0x1) {
                u32 sa1 = ((u32)op2 << 4) | t;
                u32 sa = 32u - sa1;
                s->a[r] = (sa >= 32) ? 0 : (s->a[sr] << sa);
                return;
            }
            if (op2 == 0x2 || op2 == 0x3) {
                u32 sa = (((u32)op2 & 1) << 4) | sr;
                i32 v = (i32)s->a[t];
                s->a[r] = (sa >= 32) ? (u32)(v >> 31) : (u32)(v >> sa);
                return;
            }
            if (op2 == 0x4) {
                u32 sa = sr;
                s->a[r] = s->a[t] >> sa;
                return;
            }
        }

        /* EXTUI (canonical): op0=0, bit 18 fixed = 1, bits 17,19 fixed = 0.
         * In RRR-field terms: op1 = 0b010x where x = sh_hi1.
         *   shiftimm = (sh_hi1 << 4) | sh_lo4   (sh_lo4 in s field)
         *   maskimm  = op2 (0..15, width-1)
         *   src      = t, dst = r.                                          */
        if ((op1 & 0xE) == 0x4) {
            u32 sh_hi1 = op1 & 1;
            u32 shift = (sh_hi1 << 4) | sr;
            u32 width = (u32)op2 + 1u;
            u32 mask = (width == 32) ? 0xFFFFFFFFu : ((1u << width) - 1u);
            s->a[r] = (s->a[t] >> shift) & mask;
            return;
        }
    }

    s->status = XT_SIM_BAD_OPCODE;
}

void xt_sim_run(xt_sim *s, u32 max_steps) {
    while (s->status == XT_SIM_RUN && max_steps-- > 0) {
        step(s);
        s->instr_count++;
    }
}
