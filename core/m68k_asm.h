#ifndef M68K_ASM_H
#define M68K_ASM_H

/* A tiny header-only 68000 assembler. It exists so test programs and the
 * built-in demo ROM can be written readably in C instead of as opaque hex
 * blobs — there is no host 68k toolchain in this environment.
 *
 * It supports just the instruction forms the demo / tests need, plus a
 * simple forward/backward label + branch-fixup mechanism. Everything is
 * emitted big-endian, as the 68000 expects. */

#include "m68k_types.h"
#include <string.h>

#define M68A_MAX_LABELS  64
#define M68A_MAX_FIXUPS  256

typedef struct {
    u8 *buf;
    u32 cap;
    u32 len;
    u32 org;                       /* address the code is assembled for */
    u32 label_pc[M68A_MAX_LABELS]; /* resolved address, or 0xFFFFFFFF    */
    int n_labels;
    struct { u32 site; int label; int size; } fixup[M68A_MAX_FIXUPS];
    int n_fixups;
} m68a;

static inline void m68a_init(m68a *a, u8 *buf, u32 cap, u32 org) {
    a->buf = buf; a->cap = cap; a->len = 0; a->org = org;
    a->n_labels = 0; a->n_fixups = 0;
}

static inline u32 m68a_here(const m68a *a) { return a->org + a->len; }

static inline void m68a_w16(m68a *a, u16 w) {
    if (a->len + 2 > a->cap) return;
    a->buf[a->len++] = (u8)(w >> 8);
    a->buf[a->len++] = (u8)(w & 0xFF);
}
static inline void m68a_w32(m68a *a, u32 v) {
    m68a_w16(a, (u16)(v >> 16));
    m68a_w16(a, (u16)(v & 0xFFFF));
}

/* Labels. m68a_label() reserves an id; m68a_mark() binds it to the
 * current address. */
static inline int m68a_label(m68a *a) {
    int id = a->n_labels++;
    a->label_pc[id] = 0xFFFFFFFFu;
    return id;
}
static inline void m68a_mark(m68a *a, int id) {
    a->label_pc[id] = m68a_here(a);
}

/* --- instruction emitters --------------------------------------------- */

/* MOVEQ #imm8,Dn */
static inline void m68a_moveq(m68a *a, int dn, i32 imm) {
    m68a_w16(a, (u16)(0x7000 | (dn << 9) | (imm & 0xFF)));
}
/* MOVE.L #imm32,Dn */
static inline void m68a_move_l_imm(m68a *a, int dn, u32 imm) {
    m68a_w16(a, (u16)(0x2000 | (dn << 9) | 0x3C));
    m68a_w32(a, imm);
}
/* MOVE.W #imm16,Dn */
static inline void m68a_move_w_imm(m68a *a, int dn, u16 imm) {
    m68a_w16(a, (u16)(0x3000 | (dn << 9) | 0x3C));
    m68a_w16(a, imm);
}
/* LEA (xxx).L,An */
static inline void m68a_lea_abs(m68a *a, int an, u32 addr) {
    m68a_w16(a, (u16)(0x41C0 | (an << 9) | 0x39));
    m68a_w32(a, addr);
}
/* LEA label,An — the absolute address is patched in by m68a_finish. */
static inline void m68a_lea_label(m68a *a, int an, int label) {
    m68a_w16(a, (u16)(0x41C0 | (an << 9) | 0x39));
    if (a->n_fixups < M68A_MAX_FIXUPS) {
        a->fixup[a->n_fixups].site  = a->len;
        a->fixup[a->n_fixups].label = label;
        a->fixup[a->n_fixups].size  = 32;
        a->n_fixups++;
    }
    m68a_w32(a, 0);
}
/* Emit a NUL-terminated string as raw bytes. */
static inline void m68a_string(m68a *a, const char *s) {
    while (*s && a->len < a->cap) a->buf[a->len++] = (u8)*s++;
    if (a->len < a->cap) a->buf[a->len++] = 0;
    if ((a->len & 1) && a->len < a->cap) a->buf[a->len++] = 0;  /* word-align */
}
/* MOVE.L #imm32,An  (i.e. MOVEA.L) */
static inline void m68a_movea_l_imm(m68a *a, int an, u32 imm) {
    m68a_w16(a, (u16)(0x2040 | (an << 9) | 0x3C));
    m68a_w32(a, imm);
}
/* ADD.L Dm,Dn */
static inline void m68a_add_l(m68a *a, int dn, int dm) {
    m68a_w16(a, (u16)(0xD080 | (dn << 9) | dm));
}
/* AND.L Dm,Dn */
static inline void m68a_and_l(m68a *a, int dn, int dm) {
    m68a_w16(a, (u16)(0xC080 | (dn << 9) | dm));
}
/* EOR.L Dn,Dm  (EOR Dn,<ea>) */
static inline void m68a_eor_l(m68a *a, int dn, int dm) {
    m68a_w16(a, (u16)(0xB180 | (dn << 9) | dm));
}
/* ADDQ.L #imm,Dn  (imm 1..8) */
static inline void m68a_addq_l(m68a *a, int imm, int dn) {
    m68a_w16(a, (u16)(0x5080 | ((imm & 7) << 9) | dn));
}
/* SUBQ.L #imm,Dn */
static inline void m68a_subq_l(m68a *a, int imm, int dn) {
    m68a_w16(a, (u16)(0x5180 | ((imm & 7) << 9) | dn));
}
/* ADDQ.W #imm,An */
static inline void m68a_addq_w_an(m68a *a, int imm, int an) {
    m68a_w16(a, (u16)(0x5048 | ((imm & 7) << 9) | an));
}
/* CMP.L Dm,Dn */
static inline void m68a_cmp_l(m68a *a, int dn, int dm) {
    m68a_w16(a, (u16)(0xB080 | (dn << 9) | dm));
}
/* TST.L Dn */
static inline void m68a_tst_l(m68a *a, int dn) {
    m68a_w16(a, (u16)(0x4A80 | dn));
}
/* ASL.L #cnt,Dn */
static inline void m68a_asl_l(m68a *a, int cnt, int dn) {
    m68a_w16(a, (u16)(0xE180 | ((cnt & 7) << 9) | dn));
}
/* LSR.L #cnt,Dn */
static inline void m68a_lsr_l(m68a *a, int cnt, int dn) {
    m68a_w16(a, (u16)(0xE088 | ((cnt & 7) << 9) | dn));
}
/* MOVE.B Dn,(An) */
static inline void m68a_move_b_to_an(m68a *a, int an, int dn) {
    m68a_w16(a, (u16)(0x1080 | (an << 9) | dn));
}
/* MOVE.B (An)+,Dn */
static inline void m68a_move_b_postinc(m68a *a, int dn, int an) {
    m68a_w16(a, (u16)(0x1018 | (dn << 9) | an));
}
/* MOVE.L Dn,(An) */
static inline void m68a_move_l_to_an(m68a *a, int an, int dn) {
    m68a_w16(a, (u16)(0x2080 | (an << 9) | dn));
}
/* MOVE.L (An),Dn */
static inline void m68a_move_l_from_an(m68a *a, int dn, int an) {
    m68a_w16(a, (u16)(0x2010 | (dn << 9) | an));
}
/* MOVE.L Dm,Dn */
static inline void m68a_move_l_reg(m68a *a, int dn, int dm) {
    m68a_w16(a, (u16)(0x2000 | (dn << 9) | dm));
}
static inline void m68a_nop(m68a *a) { m68a_w16(a, 0x4E71); }
static inline void m68a_rts(m68a *a) { m68a_w16(a, 0x4E75); }

/* Branches. cc: 0=BRA,1..15=Bcc condition codes (6=NE,7=EQ,...). The
 * displacement is encoded as a 16-bit form so forward refs always fit. */
static inline void m68a_branch(m68a *a, int cc, int label) {
    m68a_w16(a, (u16)(0x6000 | (cc << 8)));    /* 8-bit field 0 -> 16-bit disp */
    if (a->n_fixups < M68A_MAX_FIXUPS) {
        a->fixup[a->n_fixups].site  = a->len;  /* offset of the disp word */
        a->fixup[a->n_fixups].label = label;
        a->fixup[a->n_fixups].size  = 16;
        a->n_fixups++;
    }
    m68a_w16(a, 0);                            /* placeholder displacement */
}
static inline void m68a_bra(m68a *a, int l)  { m68a_branch(a, 0, l); }
static inline void m68a_bne(m68a *a, int l)  { m68a_branch(a, 6, l); }
static inline void m68a_beq(m68a *a, int l)  { m68a_branch(a, 7, l); }
static inline void m68a_bcs(m68a *a, int l)  { m68a_branch(a, 5, l); }
static inline void m68a_bge(m68a *a, int l)  { m68a_branch(a, 12, l); }

/* BSR to a label (16-bit form). */
static inline void m68a_bsr(m68a *a, int label) {
    m68a_w16(a, 0x6100);
    if (a->n_fixups < M68A_MAX_FIXUPS) {
        a->fixup[a->n_fixups].site  = a->len;
        a->fixup[a->n_fixups].label = label;
        a->fixup[a->n_fixups].size  = 16;
        a->n_fixups++;
    }
    m68a_w16(a, 0);
}

/* DBRA Dn,label  (DBcc with cc=F, so it only counts) */
static inline void m68a_dbra(m68a *a, int dn, int label) {
    m68a_w16(a, (u16)(0x51C8 | dn));
    if (a->n_fixups < M68A_MAX_FIXUPS) {
        a->fixup[a->n_fixups].site  = a->len;
        a->fixup[a->n_fixups].label = label;
        a->fixup[a->n_fixups].size  = 16;
        a->n_fixups++;
    }
    m68a_w16(a, 0);
}

/* STOP #imm — also handy as a hard "halt". */
static inline void m68a_stop(m68a *a, u16 sr) {
    m68a_w16(a, 0x4E72);
    m68a_w16(a, sr);
}

/* Resolve every recorded branch fixup. Call once after emitting all code. */
static inline void m68a_finish(m68a *a) {
    for (int i = 0; i < a->n_fixups; i++) {
        int lab = a->fixup[i].label;
        u32 site = a->fixup[i].site;
        u32 target = a->label_pc[lab];
        if (a->fixup[i].size == 32) {
            /* LEA absolute address — store the target outright. */
            a->buf[site + 0] = (u8)(target >> 24);
            a->buf[site + 1] = (u8)(target >> 16);
            a->buf[site + 2] = (u8)(target >> 8);
            a->buf[site + 3] = (u8)(target & 0xFF);
        } else {
            /* 68000 branch displacement is relative to (opcode word + 2),
             * which is exactly the address of this displacement word. */
            i32 disp = (i32)target - (i32)(a->org + site);
            a->buf[site]     = (u8)((u32)disp >> 8);
            a->buf[site + 1] = (u8)((u32)disp & 0xFF);
        }
    }
}

#endif
