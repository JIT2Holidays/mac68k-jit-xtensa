/* Motorola 68000 reference interpreter.
 *
 * Covers the bulk of the user- and supervisor-mode integer ISA: every
 * effective-addressing mode, MOVE/MOVEA/MOVEQ, the immediate ALU group,
 * ADD/SUB/AND/OR/EOR/CMP in all forms, ADDA/SUBA/CMPA, ADDQ/SUBQ, the
 * shift/rotate family, Bcc/BRA/BSR/DBcc/Scc, JMP/JSR/RTS/RTR/RTE, the
 * bit ops, MULU/MULS/DIVU/DIVS, MOVEM, LEA/PEA, EXT/SWAP/LINK/UNLK,
 * NEG/NEGX/NOT/CLR/TST/TAS, TRAP/exceptions and autovector interrupts.
 *
 * Not modelled: the 68010+ additions. Those
 * decode to an illegal-instruction exception.
 *
 * Cycle counts are approximate (good enough to pace the ~60 Hz VBL); the
 * goal is functional correctness, which the JIT differential test pins. */

#include "m68k_interp.h"
#include "mac_mem.h"
#include "sony.h"
#include <stdio.h>
#include <string.h>

/* ---- size helpers ----------------------------------------------------- */

static inline u32 size_mask(int sz) {
    return sz == 1 ? 0xFFu : sz == 2 ? 0xFFFFu : 0xFFFFFFFFu;
}
static inline u32 size_msb(int sz) {
    return sz == 1 ? 0x80u : sz == 2 ? 0x8000u : 0x80000000u;
}
static inline u32 sext(u32 v, int sz) {
    if (sz == 1) return (u32)(i32)(i8)(v & 0xFF);
    if (sz == 2) return (u32)(i32)(i16)(v & 0xFFFF);
    return v;
}

/* ---- CPU init --------------------------------------------------------- */

void m68k_sync_sp(m68k_cpu *cpu, bool was_super) {
    bool now_super = m68k_is_super(cpu);
    if (was_super == now_super) return;
    if (was_super) {            /* leaving supervisor: save SSP, load USP */
        cpu->ssp = cpu->a[7];
        cpu->a[7] = cpu->usp;
    } else {                    /* entering supervisor: save USP, load SSP */
        cpu->usp = cpu->a[7];
        cpu->a[7] = cpu->ssp;
    }
}

void m68k_reset(m68k_cpu *cpu, struct mac_mem *mem) {
    memset(cpu, 0, sizeof(*cpu));
    cpu->mem = mem;
    if (mem) mem->cpu = cpu;
    cpu->sr = 0x2700;           /* supervisor, interrupts masked at 7 */
    if (mem) {
        cpu->ssp = mac_read32(mem, 0);
        cpu->a[7] = cpu->ssp;
        cpu->pc = mac_read32(mem, 4);
    }
    cpu->halted = M68K_RUN;
}

/* ---- instruction stream fetch ---------------------------------------- */

static inline u16 fetch16(m68k_cpu *cpu) {
    u16 w = mac_read16(cpu->mem, cpu->pc);
    cpu->pc += 2;
    return w;
}
static inline u32 fetch32(m68k_cpu *cpu) {
    u32 v = mac_read32(cpu->mem, cpu->pc);
    cpu->pc += 4;
    return v;
}

/* ---- effective address ----------------------------------------------- */

enum { EA_DREG, EA_AREG, EA_MEM, EA_IMM };

typedef struct {
    int kind;
    int reg;     /* DREG/AREG: register index */
    u32 addr;    /* MEM: resolved address */
    u32 imm;     /* IMM: immediate value     */
} ea_t;

/* Resolve the brief-format index displacement used by modes (d8,An,Xn)
 * and (d8,PC,Xn). */
static u32 brief_index(m68k_cpu *cpu, u32 base) {
    u16 ext = fetch16(cpu);
    int ireg = (ext >> 12) & 7;
    u32 ival = (ext & 0x8000) ? cpu->a[ireg] : cpu->d[ireg];
    if (!(ext & 0x0800)) ival = (u32)(i32)(i16)(u16)ival;   /* word index */
    i32 disp = (i8)(ext & 0xFF);
    return base + ival + (u32)disp;
}

/* Decode an effective address. Consumes extension words and applies the
 * (An)+ / -(An) side effects. `sz` is the operand size in bytes. */
static ea_t ea_decode(m68k_cpu *cpu, int mode, int reg, int sz) {
    ea_t e;
    e.kind = EA_MEM; e.reg = reg; e.addr = 0; e.imm = 0;
    switch (mode) {
        case 0: e.kind = EA_DREG; return e;
        case 1: e.kind = EA_AREG; return e;
        case 2: e.addr = cpu->a[reg]; return e;
        case 3: {                              /* (An)+ */
            int step = (sz == 1 && reg == 7) ? 2 : sz;
            e.addr = cpu->a[reg];
            cpu->a[reg] += (u32)step;
            return e;
        }
        case 4: {                              /* -(An) */
            int step = (sz == 1 && reg == 7) ? 2 : sz;
            cpu->a[reg] -= (u32)step;
            e.addr = cpu->a[reg];
            return e;
        }
        case 5: {                              /* (d16,An) */
            i16 d = (i16)fetch16(cpu);
            e.addr = cpu->a[reg] + (u32)(i32)d;
            return e;
        }
        case 6:                                /* (d8,An,Xn) */
            e.addr = brief_index(cpu, cpu->a[reg]);
            return e;
        case 7:
            switch (reg) {
                case 0: e.addr = (u32)(i32)(i16)fetch16(cpu); return e;   /* (xxx).W */
                case 1: e.addr = fetch32(cpu); return e;                  /* (xxx).L */
                case 2: {                                                 /* (d16,PC) */
                    u32 base = cpu->pc;
                    i16 d = (i16)fetch16(cpu);
                    e.addr = base + (u32)(i32)d;
                    return e;
                }
                case 3: {                                                 /* (d8,PC,Xn) */
                    u32 base = cpu->pc;
                    e.addr = brief_index(cpu, base);
                    return e;
                }
                case 4:                                                   /* #imm */
                    e.kind = EA_IMM;
                    if (sz == 1)      e.imm = fetch16(cpu) & 0xFF;
                    else if (sz == 2) e.imm = fetch16(cpu);
                    else              e.imm = fetch32(cpu);
                    return e;
                default: break;
            }
            break;
        default: break;
    }
    e.kind = EA_MEM; e.addr = 0;
    return e;
}

static u32 ea_read(m68k_cpu *cpu, const ea_t *e, int sz) {
    switch (e->kind) {
        case EA_DREG: return cpu->d[e->reg] & size_mask(sz);
        case EA_AREG: return cpu->a[e->reg] & size_mask(sz);
        case EA_IMM:  return e->imm & size_mask(sz);
        default:
            if (sz == 1) return mac_read8(cpu->mem, e->addr);
            if (sz == 2) return mac_read16(cpu->mem, e->addr);
            return mac_read32(cpu->mem, e->addr);
    }
}

static void ea_write(m68k_cpu *cpu, const ea_t *e, int sz, u32 v) {
    switch (e->kind) {
        case EA_DREG: {
            u32 m = size_mask(sz);
            cpu->d[e->reg] = (cpu->d[e->reg] & ~m) | (v & m);
            return;
        }
        case EA_AREG:
            cpu->a[e->reg] = v;          /* address writes are always 32-bit */
            return;
        case EA_IMM:
            return;                       /* not writable */
        default:
            if (sz == 1)      mac_write8(cpu->mem, e->addr, (u8)v);
            else if (sz == 2) mac_write16(cpu->mem, e->addr, (u16)v);
            else              mac_write32(cpu->mem, e->addr, v);
            return;
    }
}

/* True for EA modes that name a memory location (used to gate mode-only
 * forms like the read-modify-write shifts). */
static bool ea_is_mem(int mode, int reg) {
    if (mode <= 1) return false;
    if (mode == 7 && reg == 4) return false;   /* immediate */
    return true;
}

/* ---- flags ------------------------------------------------------------ */

static void set_nz(m68k_cpu *cpu, int sz, u32 res) {
    u8 c = m68k_get_ccr(cpu) & ~(CCR_N | CCR_Z);
    if (res & size_mask(sz)) {
        if (res & size_msb(sz)) c |= CCR_N;
    } else {
        c |= CCR_Z;
    }
    m68k_set_ccr(cpu, c);
}

/* Logic ops: N,Z from result; V,C cleared; X untouched. */
static void set_flags_logic(m68k_cpu *cpu, int sz, u32 res) {
    u8 c = m68k_get_ccr(cpu) & CCR_X;
    if ((res & size_mask(sz)) == 0) c |= CCR_Z;
    if (res & size_msb(sz)) c |= CCR_N;
    m68k_set_ccr(cpu, c);
}

static void set_flags_add(m68k_cpu *cpu, int sz, u32 s, u32 d, u32 r) {
    u32 mask = size_mask(sz), msb = size_msb(sz);
    s &= mask; d &= mask; r &= mask;
    u8 c = 0;
    if (r == 0) c |= CCR_Z;
    if (r & msb) c |= CCR_N;
    bool carry = ((u64)s + (u64)d) > mask;
    if (carry) c |= CCR_C | CCR_X;
    if (((s ^ r) & (d ^ r)) & msb) c |= CCR_V;
    m68k_set_ccr(cpu, c);
}

static void set_flags_sub(m68k_cpu *cpu, int sz, u32 s, u32 d, u32 r) {
    u32 mask = size_mask(sz), msb = size_msb(sz);
    s &= mask; d &= mask; r &= mask;
    u8 c = 0;
    if (r == 0) c |= CCR_Z;
    if (r & msb) c |= CCR_N;
    if (s > d) c |= CCR_C | CCR_X;
    if (((s ^ d) & (d ^ r)) & msb) c |= CCR_V;
    m68k_set_ccr(cpu, c);
}

/* CMP: like SUB but leaves X alone and discards the result. */
static void set_flags_cmp(m68k_cpu *cpu, int sz, u32 s, u32 d, u32 r) {
    u32 mask = size_mask(sz), msb = size_msb(sz);
    s &= mask; d &= mask; r &= mask;
    u8 c = m68k_get_ccr(cpu) & CCR_X;
    if (r == 0) c |= CCR_Z;
    if (r & msb) c |= CCR_N;
    if (s > d) c |= CCR_C;
    if (((s ^ d) & (d ^ r)) & msb) c |= CCR_V;
    m68k_set_ccr(cpu, c);
}

/* ---- BCD instructions (ABCD / SBCD / NBCD) ----------------------------
 * Packed binary-coded-decimal byte arithmetic. Ported from mini vMac's
 * MINEM68K.c so that the officially-undefined N and V flags match a real
 * 68000 — the Mac ROM's date/time routines (reached from the Control
 * Panel) depend on the exact behaviour. Z is only ever cleared, never
 * set, so multi-byte BCD chains work. */
static u8 bcd_abcd(m68k_cpu *cpu, u8 src, u8 dst) {
    u8 ccr = m68k_get_ccr(cpu);
    int x = (ccr & CCR_X) ? 1 : 0;
    int flgs = (src & 0x80) != 0, flgo = (dst & 0x80) != 0;
    u16 lo = (u16)((src & 0x0F) + (dst & 0x0F) + x);
    u16 hi = (u16)((src & 0xF0) + (dst & 0xF0));
    if (lo > 9) lo = (u16)(lo + 6);
    u16 v = (u16)(hi + lo);
    int carry = (v & 0x1F0) > 0x90;
    if (carry) v = (u16)(v + 0x60);
    u8 res = (u8)v, c = (u8)(ccr & CCR_Z);
    if (carry)    c |= CCR_C | CCR_X;
    if (res != 0) c &= (u8)~CCR_Z;
    int N = (res & 0x80) != 0;
    if (N) c |= CCR_N;
    if ((flgs != flgo) && (N != flgo)) c |= CCR_V;
    m68k_set_ccr(cpu, c);
    return res;
}
static u8 bcd_sbcd(m68k_cpu *cpu, u8 src, u8 dst) {
    u8 ccr = m68k_get_ccr(cpu);
    int x = (ccr & CCR_X) ? 1 : 0;
    int flgs = (src & 0x80) != 0, flgo = (dst & 0x80) != 0;
    u16 lo = (u16)((dst & 0x0F) - (src & 0x0F) - x);
    u16 hi = (u16)((dst & 0xF0) - (src & 0xF0));
    if (lo > 9) { lo = (u16)(lo - 6); hi = (u16)(hi - 0x10); }
    u16 v = (u16)(hi + (lo & 0x0F));
    int carry = (hi & 0x1F0) > 0x90;
    if (carry) v = (u16)(v - 0x60);
    u8 res = (u8)v, c = (u8)(ccr & CCR_Z);
    if (carry)    c |= CCR_C | CCR_X;
    if (res != 0) c &= (u8)~CCR_Z;
    int N = (res & 0x80) != 0;
    if (N) c |= CCR_N;
    if ((flgs != flgo) && (N != flgo)) c |= CCR_V;
    m68k_set_ccr(cpu, c);
    return res;
}
static u8 bcd_nbcd(m68k_cpu *cpu, u8 dst) {
    u8 ccr = m68k_get_ccr(cpu);
    int x = (ccr & CCR_X) ? 1 : 0;
    u16 lo = (u16)(0 - (dst & 0x0F) - x);
    u16 hi = (u16)(0 - (dst & 0xF0));
    if (lo > 9) { lo = (u16)(lo - 6); hi = (u16)(hi - 0x10); }
    u16 v = (u16)(hi + (lo & 0x0F));
    int carry = (hi & 0x1F0) > 0x90;
    if (carry) v = (u16)(v - 0x60);
    u8 res = (u8)v, c = (u8)(ccr & CCR_Z);
    if (carry)      c |= CCR_C | CCR_X;
    if (res != 0)   c &= (u8)~CCR_Z;
    if (res & 0x80) c |= CCR_N;
    m68k_set_ccr(cpu, c);
    return res;
}

/* ---- condition codes -------------------------------------------------- */

static bool cond_true(m68k_cpu *cpu, int cc) {
    u8 f = m68k_get_ccr(cpu);
    bool C = f & CCR_C, V = f & CCR_V, Z = f & CCR_Z, N = f & CCR_N;
    switch (cc) {
        case 0:  return true;            /* T  */
        case 1:  return false;           /* F  */
        case 2:  return !C && !Z;        /* HI */
        case 3:  return C || Z;          /* LS */
        case 4:  return !C;              /* CC/HS */
        case 5:  return C;               /* CS/LO */
        case 6:  return !Z;              /* NE */
        case 7:  return Z;               /* EQ */
        case 8:  return !V;              /* VC */
        case 9:  return V;               /* VS */
        case 10: return !N;              /* PL */
        case 11: return N;               /* MI */
        case 12: return N == V;          /* GE */
        case 13: return N != V;          /* LT */
        case 14: return !Z && (N == V);  /* GT */
        case 15: return Z || (N != V);   /* LE */
    }
    return false;
}

/* ---- exceptions / interrupts ----------------------------------------- */

/* Exception trace ring — diagnostic aid for ROM bring-up. */
u32 m68k_exc_log[64][3];   /* {vector, faulting pc, count-at} */
u32 m68k_exc_n;

/* Optional debug hook, called for every line-A (Toolbox trap). */
void (*m68k_trap_hook)(m68k_cpu *cpu, u16 trap);

void m68k_exception(m68k_cpu *cpu, u32 vector) {
    m68k_exc_log[m68k_exc_n & 63][0] = vector;
    m68k_exc_log[m68k_exc_n & 63][1] = cpu->pc;
    m68k_exc_log[m68k_exc_n & 63][2] = (u32)cpu->cycles;
    m68k_exc_n++;
    bool was_super = m68k_is_super(cpu);
    u16 saved_sr = cpu->sr;
    cpu->sr |= SR_S;            /* enter supervisor */
    cpu->sr &= (u16)~SR_T;      /* clear trace */
    m68k_sync_sp(cpu, was_super);
    /* Push the short (group 1/2) exception frame: PC then SR. */
    cpu->a[7] -= 4;
    mac_write32(cpu->mem, cpu->a[7], cpu->pc);
    cpu->a[7] -= 2;
    mac_write16(cpu->mem, cpu->a[7], saved_sr);
    cpu->pc = mac_read32(cpu->mem, vector * 4u);
    cpu->cycles += 34;
}

bool m68k_poll_interrupts(m68k_cpu *cpu) {
    u32 level = cpu->pending_irq;
    if (level == 0) return false;
    u32 mask = (cpu->sr & SR_IMASK) >> 8;
    if (level != 7 && level <= mask) return false;   /* masked (7 = NMI) */

    bool was_super = m68k_is_super(cpu);
    u16 saved_sr = cpu->sr;
    cpu->sr |= SR_S;
    cpu->sr &= (u16)~SR_T;
    cpu->sr = (u16)((cpu->sr & ~SR_IMASK) | (level << 8));
    m68k_sync_sp(cpu, was_super);
    cpu->a[7] -= 4;
    mac_write32(cpu->mem, cpu->a[7], cpu->pc);
    cpu->a[7] -= 2;
    mac_write16(cpu->mem, cpu->a[7], saved_sr);
    /* Autovector: level n -> vector 24+n. */
    cpu->pc = mac_read32(cpu->mem, (24u + level) * 4u);
    cpu->stopped = 0;
    cpu->cycles += 44;
    return true;
}

/* Take an unimplemented-instruction exception. `op_pc` is the address of
 * the faulting instruction — the 68000 stacks *that* address (not the
 * next instruction) for the illegal / line-A / line-F vectors, so the
 * handler can examine the opcode. The Macintosh Toolbox/OS dispatch is
 * built on line-A traps, so this path is extremely hot during boot. */
static void m68k_unimpl(m68k_cpu *cpu, u32 op_pc, u32 vector) {
    cpu->pc = op_pc;
    m68k_exception(cpu, vector);
}
#define illegal(cpu, op)  m68k_unimpl((cpu), op_pc, 4)

/* ---- shift / rotate helpers ------------------------------------------ */

static u32 do_shift(m68k_cpu *cpu, int sz, int op, u32 val, u32 cnt) {
    u32 mask = size_mask(sz), msb = size_msb(sz);
    val &= mask;
    u8 f = m68k_get_ccr(cpu);
    bool X = f & CCR_X;
    bool last_out = false;
    bool overflow = false;
    /* op: 0=ASR,1=LSR,2=ROXR,3=ROR,4=ASL,5=LSL,6=ROXL,7=ROL */
    if (cnt == 0) {
        /* count 0: C cleared except rotates-through-X copy X into C. */
        u8 c = f & ~(CCR_C | CCR_V);
        if (op == 2 || op == 6) { if (X) c |= CCR_C; else c &= ~CCR_C; }
        m68k_set_ccr(cpu, c);
        set_nz(cpu, sz, val);
        return val;
    }
    for (u32 i = 0; i < cnt; i++) {
        switch (op) {
            case 0: /* ASR */
                last_out = val & 1;
                val = (val >> 1) | (val & msb);
                break;
            case 1: /* LSR */
                last_out = val & 1;
                val >>= 1;
                break;
            case 2: /* ROXR */ {
                bool inb = X;
                X = val & 1;
                last_out = X;
                val = (val >> 1) | (inb ? msb : 0);
                break;
            }
            case 3: /* ROR */
                last_out = val & 1;
                val = (val >> 1) | (last_out ? msb : 0);
                break;
            case 4: /* ASL */ {
                bool before = val & msb;
                last_out = before;
                val = (val << 1) & mask;
                if (((val & msb) != 0) != before) overflow = true;
                break;
            }
            case 5: /* LSL */
                last_out = (val & msb) != 0;
                val = (val << 1) & mask;
                break;
            case 6: /* ROXL */ {
                bool inb = X;
                X = (val & msb) != 0;
                last_out = X;
                val = ((val << 1) & mask) | (inb ? 1 : 0);
                break;
            }
            case 7: /* ROL */
                last_out = (val & msb) != 0;
                val = ((val << 1) & mask) | (last_out ? 1 : 0);
                break;
        }
    }
    val &= mask;
    u8 c = 0;
    if (val == 0) c |= CCR_Z;
    if (val & msb) c |= CCR_N;
    bool is_rotate_plain = (op == 3 || op == 7);
    bool is_rotate_x     = (op == 2 || op == 6);
    if (last_out) c |= CCR_C;
    if (is_rotate_x) {
        if (X) c |= CCR_X; else c &= ~CCR_X;
    } else if (!is_rotate_plain) {
        if (last_out) c |= CCR_X;       /* ASx/LSx copy C into X */
    } else {
        /* plain rotate: X preserved */
        if (f & CCR_X) c |= CCR_X;
    }
    if (overflow) c |= CCR_V;
    m68k_set_ccr(cpu, c);
    return val;
}

/* ---- BTST/BCHG/BCLR/BSET shared core --------------------------------- */

static void do_bitop(m68k_cpu *cpu, int which, int bit, ea_t *e, int mode) {
    /* Register destination -> 32-bit operand; memory -> byte operand. */
    int sz = (mode == 0) ? 4 : 1;
    bit &= (sz == 4) ? 31 : 7;
    u32 v = ea_read(cpu, e, sz);
    bool set = (v >> bit) & 1;
    u8 c = m68k_get_ccr(cpu) & ~CCR_Z;
    if (!set) c |= CCR_Z;
    m68k_set_ccr(cpu, c);
    switch (which) {
        case 0: break;                              /* BTST */
        case 1: ea_write(cpu, e, sz, v ^ (1u << bit)); break;   /* BCHG */
        case 2: ea_write(cpu, e, sz, v & ~(1u << bit)); break;  /* BCLR */
        case 3: ea_write(cpu, e, sz, v | (1u << bit)); break;   /* BSET */
    }
}

/* ---- MOVEM ------------------------------------------------------------ */

static void do_movem(m68k_cpu *cpu, u16 op) {
    int dir = (op >> 10) & 1;          /* 0: reg->mem, 1: mem->reg */
    int sz  = (op & 0x40) ? 4 : 2;
    int mode = (op >> 3) & 7;
    int reg  = op & 7;
    u16 list = fetch16(cpu);

    if (mode == 4) {                   /* -(An): registers stored A7..D0 */
        u32 addr = cpu->a[reg];
        for (int i = 0; i < 16; i++) {
            if (list & (1 << i)) {
                /* bit 0 = A7 ... bit 15 = D0 */
                int idx = 15 - i;
                u32 val = (idx < 8) ? cpu->d[idx] : cpu->a[idx - 8];
                addr -= (u32)sz;
                if (sz == 2) mac_write16(cpu->mem, addr, (u16)val);
                else         mac_write32(cpu->mem, addr, val);
            }
        }
        cpu->a[reg] = addr;
        return;
    }

    ea_t e = ea_decode(cpu, mode, reg, sz);
    u32 addr = e.addr;
    if (dir == 0) {                    /* registers -> memory, D0..A7 */
        for (int i = 0; i < 16; i++) {
            if (list & (1 << i)) {
                u32 val = (i < 8) ? cpu->d[i] : cpu->a[i - 8];
                if (sz == 2) mac_write16(cpu->mem, addr, (u16)val);
                else         mac_write32(cpu->mem, addr, val);
                addr += (u32)sz;
            }
        }
    } else {                           /* memory -> registers, D0..A7 */
        for (int i = 0; i < 16; i++) {
            if (list & (1 << i)) {
                u32 val = (sz == 2)
                    ? (u32)(i32)(i16)mac_read16(cpu->mem, addr)
                    : mac_read32(cpu->mem, addr);
                if (i < 8) cpu->d[i] = val;
                else       cpu->a[i - 8] = val;
                addr += (u32)sz;
            }
        }
        if (mode == 3) cpu->a[reg] = addr;   /* (An)+ writeback */
    }
}

/* ---- the main step ---------------------------------------------------- */

#ifdef JIT_HELPER_HISTO
u32 m68k_helper_histo[65536];
/* For top-N opcodes, also record the first PC we saw them at — helps
 * spot whether a hot histo entry comes from one site or is spread
 * over many. Keyed by op; -1 means "not seen". */
u32 m68k_helper_first_pc[65536];
#endif

/* JIT custom helper: ORI.B #imm,(d16,An) for MMIO destinations.
 *
 * The ORI.B inline arm's fast path runs entirely in registers for RAM
 * destinations. When the EA points to MMIO (e.g. VIA registers), the
 * bounds check fails and we used to bridge to m68k_step — which re-
 * fetches and re-decodes the op, ~64 LX7 of overhead. This helper does
 * just the (read byte → OR → write byte → set N/Z) work; the JIT
 * sets cpu->jit_arg1 = addr, cpu->jit_arg2 = imm before the CALLX0,
 * and handles PC/cycle advance via its own accumulator (this helper
 * touches neither pc nor cycles). */
void m68k_jit_ori_b_mmio(m68k_cpu *cpu) {
    u32 addr = cpu->jit_arg1;
    u8 imm = (u8)cpu->jit_arg2;
    u8 d = mac_read8(cpu->mem, addr);
    u8 r = (u8)(d | imm);
    mac_write8(cpu->mem, addr, r);
    u8 ccr = m68k_get_ccr(cpu) & CCR_X;
    if (r == 0)        ccr |= CCR_Z;
    if (r & 0x80)      ccr |= CCR_N;
    m68k_set_ccr(cpu, ccr);
}

/* JIT custom helper: BTST #imm,(d16,An) for MMIO destinations.
 * Tests bit at (imm & 7) of mem[addr]; sets only Z (other flags
 * preserved). PC/cycle advance is handled by the JIT's accumulator. */
void m68k_jit_btst_b_mmio(m68k_cpu *cpu) {
    u32 addr = cpu->jit_arg1;
    u32 bit = cpu->jit_arg2 & 7;
    u8 d = mac_read8(cpu->mem, addr);
    u8 ccr = m68k_get_ccr(cpu) & (u8)~CCR_Z;
    if (((d >> bit) & 1) == 0) ccr |= CCR_Z;
    m68k_set_ccr(cpu, ccr);
}

/* JIT custom MOVEM fast helpers. Skip m68k_step's decode for the
 * three shapes the JIT's MOVEM inline arms handle but defer to a
 * helper when the reglist is too big for the unrolled inline body.
 *
 * Args:  jit_arg1 = reglist (low 16 bits); jit_arg2 = An register
 *        index (0..7).  PC/cycle advance is handled by the JIT
 *        arm's emit_advance — the helper touches neither. */
void m68k_jit_movem_l_postinc_to_regs(m68k_cpu *cpu) {
    u16 list = (u16)cpu->jit_arg1;
    int an = (int)(cpu->jit_arg2 & 7);
    u32 addr = cpu->a[an];
    for (int i = 0; i < 16; i++) {
        if (list & (1 << i)) {
            u32 v = mac_read32(cpu->mem, addr);
            if (i < 8) cpu->d[i] = v;
            else       cpu->a[i - 8] = v;
            addr += 4;
        }
    }
    cpu->a[an] = addr;
}

void m68k_jit_movem_l_predec_from_regs(m68k_cpu *cpu) {
    u16 list = (u16)cpu->jit_arg1;
    int an = (int)(cpu->jit_arg2 & 7);
    u32 addr = cpu->a[an];
    for (int i = 0; i < 16; i++) {
        if (list & (1 << i)) {
            int idx = 15 - i;
            u32 v = (idx < 8) ? cpu->d[idx] : cpu->a[idx - 8];
            addr -= 4;
            mac_write32(cpu->mem, addr, v);
        }
    }
    cpu->a[an] = addr;
}

void m68k_jit_movem_w_to_mem(m68k_cpu *cpu) {
    u16 list = (u16)cpu->jit_arg1;
    int an = (int)(cpu->jit_arg2 & 7);
    u32 addr = cpu->a[an];
    for (int i = 0; i < 16; i++) {
        if (list & (1 << i)) {
            u32 v = (i < 8) ? cpu->d[i] : cpu->a[i - 8];
            mac_write16(cpu->mem, addr, (u16)v);
            addr += 2;
        }
    }
    /* (An) destination — no writeback of An. */
}

void m68k_jit_movem_l_to_mem(m68k_cpu *cpu) {
    u16 list = (u16)cpu->jit_arg1;
    int an = (int)(cpu->jit_arg2 & 7);
    u32 addr = cpu->a[an];
    for (int i = 0; i < 16; i++) {
        if (list & (1 << i)) {
            u32 v = (i < 8) ? cpu->d[i] : cpu->a[i - 8];
            mac_write32(cpu->mem, addr, v);
            addr += 4;
        }
    }
    /* (An) destination — no writeback of An. */
}

/* JIT custom helper: MOVE.W (An)+,Dn for MMIO destinations.
 * Args: jit_arg2 packed = dn | (an << 4). jit_arg1 unused.
 * Reads word from cpu->a[an], writes to low 16 of cpu->d[dn],
 * post-increments An by 2, sets N/Z (V/C=0, X preserved). */
void m68k_jit_move_w_postinc_to_dn(m68k_cpu *cpu) {
    int dn = (int)(cpu->jit_arg2 & 7);
    int an = (int)((cpu->jit_arg2 >> 4) & 7);
    u32 addr = cpu->a[an];
    u16 v = mac_read16(cpu->mem, addr);
    cpu->d[dn] = (cpu->d[dn] & 0xFFFF0000u) | v;
    cpu->a[an] = addr + 2;
    u8 ccr = m68k_get_ccr(cpu) & CCR_X;
    if (v == 0)        ccr |= CCR_Z;
    if (v & 0x8000)    ccr |= CCR_N;
    m68k_set_ccr(cpu, ccr);
}

/* M6.132 — fast helpers for bench's 21K-hit RTS/BSR.S/BSR.W MMIO
 * fallback patterns. These replace m68k_step in the slow-path bridge
 * of the RTS/BSR inline arms.
 *
 * Benefit:
 *   - Skip m68k_step's full decode (saves ~30+ LX7 on real ESP32).
 *   - Don't increment cpu->instrs → real_helpers (= cpu.instrs) drops
 *     visibly in real_lx7_per_cyc.
 *   - Helpers DO NOT touch any guest D/A reg other than A7. The JIT
 *     arm sets g_helper_touched_mask = (1 << G_A(7)) so cache reload
 *     only loads the A7 slot.
 *
 * NOTE: PC/cycle advance is owned by the JIT's compile-time accumulator
 * (emit_advance); these helpers DO NOT touch cpu->cycles. RTS does
 * write cpu->pc directly (the popped value). BSR helpers also write
 * cpu->pc directly (the branch target). */
void m68k_jit_rts_mmio(m68k_cpu *cpu) {
    /* RTS: pop 32-bit PC from (SP), SP += 4. */
    cpu->pc = mac_read32(cpu->mem, cpu->a[7]);
    cpu->a[7] += 4;
}

void m68k_jit_bsr_s_mmio(m68k_cpu *cpu) {
    /* BSR.S: SP -= 4, push (cpu->pc + 2) to (SP), pc = target.
     * Caller's emit_advance_flush has set cpu->pc = source_pc, so the
     * push value is source_pc + 2 = BSR.S return PC. The JIT arm passes
     * target_pc via jit_arg1. */
    u32 return_pc = cpu->pc + 2;
    cpu->a[7] -= 4;
    mac_write32(cpu->mem, cpu->a[7], return_pc);
    cpu->pc = cpu->jit_arg1;
}

void m68k_jit_bsr_w_mmio(m68k_cpu *cpu) {
    /* BSR.W: same shape but BSR.W is 4 bytes long, so return_pc =
     * source_pc + 4. */
    u32 return_pc = cpu->pc + 4;
    cpu->a[7] -= 4;
    mac_write32(cpu->mem, cpu->a[7], return_pc);
    cpu->pc = cpu->jit_arg1;
}

void m68k_jit_move_b_dn_to_addr_mmio(m68k_cpu *cpu) {
    /* MOVE.B Dn,addr — caller pre-computes dst addr in jit_arg1;
     * jit_arg2 holds the source Dn (0..7).
     *
     * Used by MOVE.B Dn,(An) and MOVE.B Dn,(d16,An) inline arms. */
    int dn = (int)(cpu->jit_arg2 & 7);
    u32 addr = cpu->jit_arg1;
    u8 v = (u8)(cpu->d[dn] & 0xFF);
    mac_write8(cpu->mem, addr, v);
    u8 ccr = m68k_get_ccr(cpu) & CCR_X;
    if (v == 0)        ccr |= CCR_Z;
    if (v & 0x80)      ccr |= CCR_N;
    m68k_set_ccr(cpu, ccr);
}

void m68k_jit_move_l_dn_to_anpi_mmio(m68k_cpu *cpu) {
    /* MOVE.L Dn|Am,(An)+ — bench/boot-hot 0x24C3 (MOVE.L D3,(A2)+) at
     * 5472 hits / 100 M cyc when An→MMIO.
     *
     * jit_arg2 packed: bits 0-2 = src_reg, bits 4-6 = dst_an,
     *                  bit 8 = src_is_an. jit_arg1 unused. */
    int src = (int)(cpu->jit_arg2 & 7);
    int an  = (int)((cpu->jit_arg2 >> 4) & 7);
    bool src_is_an = (cpu->jit_arg2 >> 8) & 1;
    u32 v = src_is_an ? cpu->a[src] : cpu->d[src];
    u32 addr = cpu->a[an];
    mac_write32(cpu->mem, addr, v);
    cpu->a[an] = addr + 4;
    u8 ccr = m68k_get_ccr(cpu) & CCR_X;
    if (v == 0)             ccr |= CCR_Z;
    if (v & 0x80000000u)    ccr |= CCR_N;
    m68k_set_ccr(cpu, ccr);
}

void m68k_jit_move_b_addr_to_dn_mmio(m68k_cpu *cpu) {
    /* Generic MOVE.B src→Dn fast helper. Caller pre-computes the source
     * address in jit_arg1; jit_arg2 holds the destination Dn (0..7).
     *
     * Used by inline arms for:
     *   MOVE.B (d16,An),Dn — addr = An + sext16(d16)
     *   MOVE.B (An),Dn     — addr = An
     *   MOVE.B (xxx).W,Dn  — addr = sext16(ext)
     *
     * Boot's 0x10A8 (12 K), 0x1211 (6 K), 0x1082 (3 K), 0x1411 (3 K)
     * fire this when their RAM-or-ROM bounds check fails (MMIO target).
     *
     * Reads byte from mac_read8(jit_arg1); merges into low 8 of
     * cpu->d[dn] preserving high 24. Sets MOVE-family CCR (N/Z from
     * byte sign, V/C=0, X preserved). Does NOT advance pc/cycles
     * (JIT's emit_advance owns them); does NOT increment cpu->instrs. */
    int dn = (int)(cpu->jit_arg2 & 7);
    u32 addr = cpu->jit_arg1;
    u8 v = mac_read8(cpu->mem, addr);
    cpu->d[dn] = (cpu->d[dn] & 0xFFFFFF00u) | v;
    u8 ccr = m68k_get_ccr(cpu) & CCR_X;
    if (v == 0)        ccr |= CCR_Z;
    if (v & 0x80)      ccr |= CCR_N;
    m68k_set_ccr(cpu, ccr);
}

void m68k_jit_move_l_postinc_to_dn_mmio(m68k_cpu *cpu) {
    /* MOVE.L (An)+,Dn — bench-hot 0x201F (MOVE.L (A7)+,D0) at 21 808
     * helpers / 100 M cyc when SP→MMIO. Args: jit_arg2 packed = dn |
     * (an << 4). Reads .L from cpu->a[an], writes to cpu->d[dn], post-
     * increments An by 4. MOVE-family flags (N/Z from .L, V/C=0,
     * X preserved). */
    int dn = (int)(cpu->jit_arg2 & 7);
    int an = (int)((cpu->jit_arg2 >> 4) & 7);
    u32 addr = cpu->a[an];
    u32 v = mac_read32(cpu->mem, addr);
    cpu->d[dn] = v;
    cpu->a[an] = addr + 4;
    u8 ccr = m68k_get_ccr(cpu) & CCR_X;
    if (v == 0)             ccr |= CCR_Z;
    if (v & 0x80000000u)    ccr |= CCR_N;
    m68k_set_ccr(cpu, ccr);
}

void m68k_step(m68k_cpu *cpu) {
    /* Once the CPU has halted (e.g. the guest wrote the debug exit port),
     * further steps are no-ops. This keeps a JIT block — which may contain
     * instructions after the halting one — from diverging from the
     * interpreter's run loop, which stops the instant `halted` is set. */
    if (cpu->halted) return;
    cpu->instrs++;
    u32 op_pc = cpu->pc;
    u16 op = fetch16(cpu);
#ifdef JIT_HELPER_HISTO
    if (m68k_helper_histo[op] == 0) m68k_helper_first_pc[op] = op_pc;
    m68k_helper_histo[op]++;
#endif
    cpu->cycles += 4;
    int top = (op >> 12) & 0xF;

    switch (top) {

    /* ---- 0000: immediate ALU + bit ops ------------------------------- */
    case 0x0: {
        int op9 = (op >> 9) & 7;
        int szf = (op >> 6) & 3;
        int mode = (op >> 3) & 7, reg = op & 7;
        /* MOVEP — 0000 ddd1 ooo 001 aaa. It shares the dynamic-bit-op
         * opcode space but is distinguished by the An addressing mode
         * (bit ops cannot target an address register). Transfers Dn to/
         * from alternating bytes of d16(Ay). */
        if (((op >> 8) & 1) && mode == 1) {
            int dn = (op >> 9) & 7, an = reg;
            int opmode = (op >> 6) & 7;   /* 4:W m->r 5:L m->r 6:W r->m 7:L r->m */
            i16 d = (i16)fetch16(cpu);
            u32 addr = cpu->a[an] + (u32)(i32)d;
            bool is_long = (opmode & 1) != 0;
            bool to_mem  = (opmode & 2) != 0;
            int nbytes = is_long ? 4 : 2;
            if (to_mem) {
                for (int i = 0; i < nbytes; i++)
                    mac_write8(cpu->mem, addr + (u32)i * 2u,
                               (u8)(cpu->d[dn] >> ((nbytes - 1 - i) * 8)));
            } else {
                u32 v = 0;
                for (int i = 0; i < nbytes; i++)
                    v = (v << 8) | mac_read8(cpu->mem, addr + (u32)i * 2u);
                if (is_long) cpu->d[dn] = v;
                else cpu->d[dn] = (cpu->d[dn] & 0xFFFF0000u) | (v & 0xFFFFu);
            }
            cpu->cycles += is_long ? 24 : 16;
            return;
        }
        if (((op >> 8) & 1) || op9 == 4) {
            /* Bit ops. Source bit number: immediate (op8=0) or Dn (op8=1). */
            int dyn = (op >> 8) & 1;
            int which = (op >> 6) & 3;       /* BTST/BCHG/BCLR/BSET */
            int bit;
            if (dyn) bit = (int)cpu->d[(op >> 9) & 7];
            else     bit = (int)(fetch16(cpu) & 0xFF);
            ea_t e = ea_decode(cpu, mode, reg, (mode == 0) ? 4 : 1);
            do_bitop(cpu, which, bit, &e, mode);
            cpu->cycles += 4;
            return;
        }
        /* ORI/ANDI/SUBI/ADDI/-/EORI/CMPI #imm,<ea> */
        int sz = szf == 0 ? 1 : szf == 1 ? 2 : 4;
        u32 imm = (sz == 4) ? fetch32(cpu) : (fetch16(cpu) & size_mask(sz));
        /* CCR/SR forms: ORI/ANDI/EORI to CCR (#imm,SR) — mode 7 reg 4. */
        if (mode == 7 && reg == 4 && (op9 == 0 || op9 == 1 || op9 == 5)) {
            bool to_sr = (sz == 2);
            u16 cur = to_sr ? cpu->sr : (u16)m68k_get_ccr(cpu);
            u16 v = (u16)imm;
            u16 res = op9 == 0 ? (cur | v) : op9 == 1 ? (cur & v) : (cur ^ v);
            if (to_sr) {
                bool was = m68k_is_super(cpu);
                cpu->sr = res;
                m68k_sync_sp(cpu, was);
            } else {
                m68k_set_ccr(cpu, (u8)res);
            }
            return;
        }
        ea_t e = ea_decode(cpu, mode, reg, sz);
        u32 d = ea_read(cpu, &e, sz);
        u32 r = 0;
        switch (op9) {
            case 0: r = d | imm; ea_write(cpu, &e, sz, r); set_flags_logic(cpu, sz, r); break;
            case 1: r = d & imm; ea_write(cpu, &e, sz, r); set_flags_logic(cpu, sz, r); break;
            case 2: r = d - imm; ea_write(cpu, &e, sz, r); set_flags_sub(cpu, sz, imm, d, r); break;
            case 3: r = d + imm; ea_write(cpu, &e, sz, r); set_flags_add(cpu, sz, imm, d, r); break;
            case 5: r = d ^ imm; ea_write(cpu, &e, sz, r); set_flags_logic(cpu, sz, r); break;
            case 6: r = d - imm; set_flags_cmp(cpu, sz, imm, d, r); break;   /* CMPI */
            default: illegal(cpu, op); return;
        }
        cpu->cycles += 4;
        return;
    }

    /* ---- 0001/0010/0011: MOVE / MOVEA -------------------------------- */
    case 0x1: case 0x2: case 0x3: {
        int sz = top == 1 ? 1 : top == 3 ? 2 : 4;
        int src_mode = (op >> 3) & 7, src_reg = op & 7;
        int dst_reg  = (op >> 9) & 7, dst_mode = (op >> 6) & 7;
        ea_t s = ea_decode(cpu, src_mode, src_reg, sz);
        u32 v = ea_read(cpu, &s, sz);
        if (dst_mode == 1) {                       /* MOVEA */
            cpu->a[dst_reg] = sext(v, sz);         /* word form sign-extends */
            cpu->cycles += 4;
            return;
        }
        ea_t d = ea_decode(cpu, dst_mode, dst_reg, sz);
        ea_write(cpu, &d, sz, v);
        set_flags_logic(cpu, sz, v);
        cpu->cycles += 4;
        return;
    }

    /* ---- 0100: misc -------------------------------------------------- */
    case 0x4: {
        /* Fixed-encoding instructions first. */
        if (op == 0x4E71) { return; }                       /* NOP */
        if (op == 0x4E70) { cpu->halted = M68K_HALT_RESET; cpu->chain_budget = 0; return; } /* RESET -> stop */
        if (op == 0x4E73) {                                  /* RTE */
            bool was = m68k_is_super(cpu);
            u16 sr = mac_read16(cpu->mem, cpu->a[7]); cpu->a[7] += 2;
            u32 pc = mac_read32(cpu->mem, cpu->a[7]); cpu->a[7] += 4;
            cpu->sr = sr; cpu->pc = pc;
            m68k_sync_sp(cpu, was);
            return;
        }
        if (op == 0x4E77) {                                  /* RTR */
            u16 ccr = mac_read16(cpu->mem, cpu->a[7]); cpu->a[7] += 2;
            u32 pc = mac_read32(cpu->mem, cpu->a[7]); cpu->a[7] += 4;
            m68k_set_ccr(cpu, (u8)ccr); cpu->pc = pc;
            return;
        }
        if (op == 0x4E75) {                                  /* RTS */
            cpu->pc = mac_read32(cpu->mem, cpu->a[7]);
            cpu->a[7] += 4;
            cpu->cycles += 12;
            return;
        }
        if (op == 0x4E76) {                                  /* TRAPV */
            if (m68k_get_ccr(cpu) & CCR_V) m68k_exception(cpu, 7);
            return;
        }
        if ((op & 0xFFF8) == 0x4E50) {                       /* LINK An,#d16 */
            int an = op & 7;
            i16 d = (i16)fetch16(cpu);
            cpu->a[7] -= 4;
            mac_write32(cpu->mem, cpu->a[7], cpu->a[an]);
            cpu->a[an] = cpu->a[7];
            cpu->a[7] += (u32)(i32)d;
            cpu->cycles += 12;
            return;
        }
        if ((op & 0xFFF8) == 0x4E58) {                       /* UNLK An */
            int an = op & 7;
            cpu->a[7] = cpu->a[an];
            cpu->a[an] = mac_read32(cpu->mem, cpu->a[7]);
            cpu->a[7] += 4;
            cpu->cycles += 12;
            return;
        }
        if ((op & 0xFFF0) == 0x4E40) {                       /* TRAP #vector */
            m68k_exception(cpu, 32u + (op & 0xF));
            return;
        }
        if ((op & 0xFFF8) == 0x4E60) {                       /* MOVE An,USP */
            cpu->usp = cpu->a[op & 7]; return;
        }
        if ((op & 0xFFF8) == 0x4E68) {                       /* MOVE USP,An */
            cpu->a[op & 7] = cpu->usp; return;
        }
        if (op == 0x4E72) {                                  /* STOP #imm */
            u16 sr = fetch16(cpu);
            bool was = m68k_is_super(cpu);
            cpu->sr = sr;
            m68k_sync_sp(cpu, was);
            cpu->stopped = 1;
            /* Break native JIT chaining (ESP32, M6.54): the chain epilogue
             * does not check cpu->stopped, so without this the next chained
             * block would execute when we should be idling waiting for an
             * IRQ. The dispatcher's STOP wait happens only between
             * non-chained returns. */
            cpu->chain_budget = 0;
            return;
        }
        if ((op & 0xFFF8) == 0x4840) {                       /* SWAP Dn */
            int dn = op & 7;
            u32 v = cpu->d[dn];
            v = (v >> 16) | (v << 16);
            cpu->d[dn] = v;
            set_flags_logic(cpu, 4, v);
            return;
        }
        if ((op & 0xFFB8) == 0x4880) {                       /* EXT */
            int dn = op & 7;
            if (op & 0x40) {   /* EXT.L: word -> long */
                cpu->d[dn] = (u32)(i32)(i16)(u16)cpu->d[dn];
                set_flags_logic(cpu, 4, cpu->d[dn]);
            } else {           /* EXT.W: byte -> word */
                u32 v = (u32)(i32)(i8)(u8)cpu->d[dn];
                cpu->d[dn] = (cpu->d[dn] & 0xFFFF0000u) | (v & 0xFFFFu);
                set_flags_logic(cpu, 2, v);
            }
            return;
        }
        if ((op & 0xFFC0) == 0x4800) {                       /* NBCD <ea> */
            int mode = (op >> 3) & 7, reg = op & 7;
            ea_t e = ea_decode(cpu, mode, reg, 1);
            u8 d = (u8)ea_read(cpu, &e, 1);
            ea_write(cpu, &e, 1, bcd_nbcd(cpu, d));
            cpu->cycles += 6;
            return;
        }
        /* JMP / JSR / PEA — selector in bits 11..6. */
        if ((op & 0xFFC0) == 0x4EC0) {                       /* JMP */
            int mode = (op >> 3) & 7, reg = op & 7;
            ea_t e = ea_decode(cpu, mode, reg, 4);
            cpu->pc = e.addr;
            cpu->cycles += 8;
            return;
        }
        if ((op & 0xFFC0) == 0x4E80) {                       /* JSR */
            int mode = (op >> 3) & 7, reg = op & 7;
            ea_t e = ea_decode(cpu, mode, reg, 4);
            cpu->a[7] -= 4;
            mac_write32(cpu->mem, cpu->a[7], cpu->pc);
            cpu->pc = e.addr;
            cpu->cycles += 16;
            return;
        }
        if ((op & 0xFFC0) == 0x4840) {                       /* PEA */
            int mode = (op >> 3) & 7, reg = op & 7;
            ea_t e = ea_decode(cpu, mode, reg, 4);
            cpu->a[7] -= 4;
            mac_write32(cpu->mem, cpu->a[7], e.addr);
            cpu->cycles += 12;
            return;
        }
        if (((op >> 6) & 7) == 7) {                          /* LEA An,<ea> */
            int an = (op >> 9) & 7;
            int mode = (op >> 3) & 7, reg = op & 7;
            ea_t e = ea_decode(cpu, mode, reg, 4);
            cpu->a[an] = e.addr;
            cpu->cycles += 4;
            return;
        }
        if (((op >> 6) & 7) == 6 || ((op >> 6) & 7) == 5) {  /* CHK */
            int dn = (op >> 9) & 7;
            int mode = (op >> 3) & 7, reg = op & 7;
            int sz = (((op >> 6) & 7) == 6) ? 2 : 4;
            ea_t e = ea_decode(cpu, mode, reg, sz);
            i32 bound = (i32)sext(ea_read(cpu, &e, sz), sz);
            i32 v = (i32)sext(cpu->d[dn], sz);
            if (v < 0 || v > bound) m68k_exception(cpu, 6);
            return;
        }
        /* MOVEM. */
        if ((op & 0xFB80) == 0x4880) { do_movem(cpu, op); cpu->cycles += 16; return; }
        /* MOVE from/to SR/CCR. */
        if ((op & 0xFFC0) == 0x40C0) {                       /* MOVE SR,<ea> */
            int mode = (op >> 3) & 7, reg = op & 7;
            ea_t e = ea_decode(cpu, mode, reg, 2);
            ea_write(cpu, &e, 2, cpu->sr);
            return;
        }
        if ((op & 0xFFC0) == 0x44C0) {                       /* MOVE <ea>,CCR */
            int mode = (op >> 3) & 7, reg = op & 7;
            ea_t e = ea_decode(cpu, mode, reg, 2);
            m68k_set_ccr(cpu, (u8)ea_read(cpu, &e, 2));
            return;
        }
        if ((op & 0xFFC0) == 0x46C0) {                       /* MOVE <ea>,SR */
            int mode = (op >> 3) & 7, reg = op & 7;
            ea_t e = ea_decode(cpu, mode, reg, 2);
            bool was = m68k_is_super(cpu);
            cpu->sr = (u16)ea_read(cpu, &e, 2);
            m68k_sync_sp(cpu, was);
            return;
        }
        /* NEGX/CLR/NEG/NOT/TST/TAS — selector bits 11..8. */
        {
            int sel = (op >> 8) & 0xF;
            int szf = (op >> 6) & 3;
            int mode = (op >> 3) & 7, reg = op & 7;
            if (szf != 3 && (sel == 0x0 || sel == 0x2 || sel == 0x4 ||
                             sel == 0x6 || sel == 0xA)) {
                int sz = szf == 0 ? 1 : szf == 1 ? 2 : 4;
                ea_t e = ea_decode(cpu, mode, reg, sz);
                if (sel == 0xA) {                            /* TST */
                    u32 v = ea_read(cpu, &e, sz);
                    set_flags_logic(cpu, sz, v);
                    return;
                }
                u32 d = ea_read(cpu, &e, sz);
                u32 r = 0;
                if (sel == 0x4) {                            /* NEG */
                    r = (u32)(0 - d);
                    ea_write(cpu, &e, sz, r);
                    set_flags_sub(cpu, sz, d, 0, r);
                } else if (sel == 0x0) {                     /* NEGX */
                    bool X = m68k_get_ccr(cpu) & CCR_X;
                    bool zold = m68k_get_ccr(cpu) & CCR_Z;
                    r = (u32)(0 - d - (X ? 1u : 0u));
                    ea_write(cpu, &e, sz, r);
                    u32 mask = size_mask(sz), msb = size_msb(sz);
                    u32 dm = d & mask, rm = r & mask;
                    u8 c = 0;
                    if (rm & msb)        c |= CCR_N;
                    /* X-form Z is sticky: cleared on a nonzero result,
                     * left unchanged on a zero result. */
                    if (zold && rm == 0) c |= CCR_Z;
                    if (dm != 0 || X)    c |= CCR_C | CCR_X;
                    /* Overflow: operand and result both negative — i.e.
                     * negating the most-negative value. */
                    if ((dm & rm) & msb) c |= CCR_V;
                    m68k_set_ccr(cpu, c);
                } else if (sel == 0x6) {                     /* NOT */
                    r = ~d;
                    ea_write(cpu, &e, sz, r);
                    set_flags_logic(cpu, sz, r);
                } else if (sel == 0x2) {                     /* CLR */
                    ea_write(cpu, &e, sz, 0);
                    m68k_set_ccr(cpu, (u8)((m68k_get_ccr(cpu) & CCR_X) | CCR_Z));
                }
                cpu->cycles += 4;
                return;
            }
            if (sel == 0xA && szf == 3) {                    /* TAS <ea> */
                ea_t e = ea_decode(cpu, mode, reg, 1);
                u32 v = ea_read(cpu, &e, 1);
                set_flags_logic(cpu, 1, v);
                ea_write(cpu, &e, 1, v | 0x80u);
                return;
            }
        }
        illegal(cpu, op);
        return;
    }

    /* ---- 0101: ADDQ/SUBQ/Scc/DBcc ------------------------------------ */
    case 0x5: {
        int szf = (op >> 6) & 3;
        int mode = (op >> 3) & 7, reg = op & 7;
        if (szf == 3) {
            int cc = (op >> 8) & 0xF;
            if (mode == 1) {                                 /* DBcc */
                int dn = reg;
                i16 disp = (i16)fetch16(cpu);
                if (!cond_true(cpu, cc)) {
                    u16 c = (u16)(cpu->d[dn] & 0xFFFF);
                    c--;
                    cpu->d[dn] = (cpu->d[dn] & 0xFFFF0000u) | c;
                    if (c != 0xFFFF) cpu->pc = op_pc + 2 + (u32)(i32)disp;
                }
                cpu->cycles += 10;
                return;
            }
            /* Scc <ea> */
            ea_t e = ea_decode(cpu, mode, reg, 1);
            ea_write(cpu, &e, 1, cond_true(cpu, cc) ? 0xFF : 0x00);
            cpu->cycles += 4;
            return;
        }
        int sz = szf == 0 ? 1 : szf == 1 ? 2 : 4;
        int data = (op >> 9) & 7; if (data == 0) data = 8;
        bool sub = (op >> 8) & 1;
        ea_t e = ea_decode(cpu, mode, reg, sz);
        if (e.kind == EA_AREG) {              /* ADDQ/SUBQ to An: no flags */
            cpu->a[reg] += sub ? (u32)-data : (u32)data;
            cpu->cycles += 4;
            return;
        }
        u32 d = ea_read(cpu, &e, sz);
        u32 r = sub ? d - (u32)data : d + (u32)data;
        ea_write(cpu, &e, sz, r);
        if (sub) set_flags_sub(cpu, sz, (u32)data, d, r);
        else     set_flags_add(cpu, sz, (u32)data, d, r);
        cpu->cycles += 4;
        return;
    }

    /* ---- 0110: Bcc / BRA / BSR --------------------------------------- */
    case 0x6: {
        int cc = (op >> 8) & 0xF;
        i32 disp = (i8)(op & 0xFF);
        u32 base = cpu->pc;             /* PC after the opcode word */
        if ((op & 0xFF) == 0x00) {
            disp = (i16)fetch16(cpu);
        } else if ((op & 0xFF) == 0xFF) {
            disp = (i32)fetch32(cpu);   /* 68020 long form */
        }
        u32 target = base + (u32)disp;
        if (cc == 1) {                  /* BSR */
            cpu->a[7] -= 4;
            mac_write32(cpu->mem, cpu->a[7], cpu->pc);
            cpu->pc = target;
            cpu->cycles += 18;
            return;
        }
        if (cc == 0 || cond_true(cpu, cc)) {   /* BRA or taken Bcc */
            cpu->pc = target;
            cpu->cycles += 10;
        } else {
            cpu->cycles += 8;
        }
        return;
    }

    /* ---- 0111: MOVEQ ------------------------------------------------- */
    case 0x7: {
        int dn = (op >> 9) & 7;
        u32 v = (u32)(i32)(i8)(op & 0xFF);
        cpu->d[dn] = v;
        set_flags_logic(cpu, 4, v);
        return;
    }

    /* ---- 1000: OR / DIVU / DIVS -------------------------------------- */
    case 0x8: {
        int dn = (op >> 9) & 7;
        int szf = (op >> 6) & 3;
        int mode = (op >> 3) & 7, reg = op & 7;
        if (szf == 3) {                         /* DIVU / DIVS */
            bool sgn = (op >> 8) & 1;
            ea_t e = ea_decode(cpu, mode, reg, 2);
            u32 div = ea_read(cpu, &e, 2);
            if (div == 0) { m68k_exception(cpu, 5); return; }
            if (!sgn) {
                u32 dv = cpu->d[dn];
                u32 q = dv / div, r = dv % div;
                if (q > 0xFFFF) {
                    m68k_set_ccr(cpu, (u8)((m68k_get_ccr(cpu) & CCR_X) | CCR_V));
                } else {
                    cpu->d[dn] = ((r & 0xFFFF) << 16) | (q & 0xFFFF);
                    set_flags_logic(cpu, 2, q);
                }
            } else {
                i32 dv = (i32)cpu->d[dn];
                i32 dvs = (i32)(i16)(u16)div;
                i32 q = dv / dvs, r = dv % dvs;
                if (q > 32767 || q < -32768) {
                    m68k_set_ccr(cpu, (u8)((m68k_get_ccr(cpu) & CCR_X) | CCR_V));
                } else {
                    cpu->d[dn] = (((u32)r & 0xFFFF) << 16) | ((u32)q & 0xFFFF);
                    set_flags_logic(cpu, 2, (u32)q);
                }
            }
            cpu->cycles += 140;
            return;
        }
        /* SBCD: 1000 ddd 1 0000 m rrr */
        if ((op & 0x01F0) == 0x0100) {
            int sx = op & 7, dx = (op >> 9) & 7;
            if ((op >> 3) & 1) {                /* -(Ay),-(Ax) */
                ea_t se = ea_decode(cpu, 4, sx, 1);
                u8 s = (u8)ea_read(cpu, &se, 1);
                ea_t de = ea_decode(cpu, 4, dx, 1);
                u8 d = (u8)ea_read(cpu, &de, 1);
                ea_write(cpu, &de, 1, bcd_sbcd(cpu, s, d));
            } else {                            /* Dy,Dx */
                u8 r = bcd_sbcd(cpu, (u8)cpu->d[sx], (u8)cpu->d[dx]);
                cpu->d[dx] = (cpu->d[dx] & ~0xFFu) | r;
            }
            cpu->cycles += 6;
            return;
        }
        int sz = szf == 0 ? 1 : szf == 1 ? 2 : 4;
        bool to_ea = (op >> 8) & 1;
        ea_t e = ea_decode(cpu, mode, reg, sz);
        if (to_ea) {
            u32 d = ea_read(cpu, &e, sz);
            u32 r = d | (cpu->d[dn] & size_mask(sz));
            ea_write(cpu, &e, sz, r);
            set_flags_logic(cpu, sz, r);
        } else {
            u32 r = (cpu->d[dn] & size_mask(sz)) | ea_read(cpu, &e, sz);
            cpu->d[dn] = (cpu->d[dn] & ~size_mask(sz)) | (r & size_mask(sz));
            set_flags_logic(cpu, sz, r);
        }
        cpu->cycles += 4;
        return;
    }

    /* ---- 1001/1101: SUB / ADD (+ A forms, + X forms) ----------------- */
    case 0x9: case 0xD: {
        bool is_add = (top == 0xD);
        int dn = (op >> 9) & 7;
        int szf = (op >> 6) & 3;
        int mode = (op >> 3) & 7, reg = op & 7;
        if (szf == 3) {                         /* ADDA / SUBA */
            int sz = (op & 0x0100) ? 4 : 2;
            ea_t e = ea_decode(cpu, mode, reg, sz);
            u32 v = sext(ea_read(cpu, &e, sz), sz);
            cpu->a[dn] += is_add ? v : (u32)-v;
            cpu->cycles += 8;
            return;
        }
        int sz = szf == 0 ? 1 : szf == 1 ? 2 : 4;
        bool to_ea = (op >> 8) & 1;
        /* ADDX/SUBX: to_ea form with mode 0 or 1 (Dn,Dn or -(An),-(An)). */
        if (to_ea && (mode == 0 || mode == 1)) {
            bool X = m68k_get_ccr(cpu) & CCR_X;
            bool zold = m68k_get_ccr(cpu) & CCR_Z;
            u32 s, d, r;
            if (mode == 0) {
                s = cpu->d[reg] & size_mask(sz);
                d = cpu->d[dn]  & size_mask(sz);
            } else {
                int step = (sz == 1 && reg == 7) ? 2 : sz;
                cpu->a[reg] -= (u32)step;
                cpu->a[dn]  -= (u32)((sz == 1 && dn == 7) ? 2 : sz);
                s = (sz==1)?mac_read8(cpu->mem,cpu->a[reg]):
                    (sz==2)?mac_read16(cpu->mem,cpu->a[reg]):mac_read32(cpu->mem,cpu->a[reg]);
                d = (sz==1)?mac_read8(cpu->mem,cpu->a[dn]):
                    (sz==2)?mac_read16(cpu->mem,cpu->a[dn]):mac_read32(cpu->mem,cpu->a[dn]);
            }
            /* ADDX/SUBX flags differ from plain ADD/SUB in two ways, so
             * they are computed here rather than via set_flags_add/sub:
             *   - carry/borrow (and hence X) must include the X input;
             *   - Z is sticky: cleared on a nonzero result, left
             *     unchanged on a zero result. */
            {
                u32 mask = size_mask(sz), msb = size_msb(sz);
                u32 sm, dm, rm;
                bool carry, ovfl;
                if (is_add) {
                    r = d + s + (X ? 1u : 0u);
                    sm = s & mask; dm = d & mask; rm = r & mask;
                    carry = ((u64)sm + (u64)dm + (X ? 1u : 0u)) > mask;
                    ovfl  = (((sm ^ rm) & (dm ^ rm)) & msb) != 0;
                } else {
                    r = d - s - (X ? 1u : 0u);
                    sm = s & mask; dm = d & mask; rm = r & mask;
                    carry = ((u64)sm + (X ? 1u : 0u)) > (u64)dm;
                    ovfl  = (((sm ^ dm) & (dm ^ rm)) & msb) != 0;
                }
                u8 c = 0;
                if (rm & msb)        c |= CCR_N;
                if (zold && rm == 0) c |= CCR_Z;       /* sticky Z */
                if (carry)           c |= CCR_C | CCR_X;
                if (ovfl)            c |= CCR_V;
                m68k_set_ccr(cpu, c);
            }
            if (mode == 0)
                cpu->d[dn] = (cpu->d[dn] & ~size_mask(sz)) | (r & size_mask(sz));
            else
                ea_write(cpu, &(ea_t){.kind=EA_MEM,.addr=cpu->a[dn]}, sz, r);
            cpu->cycles += 8;
            return;
        }
        ea_t e = ea_decode(cpu, mode, reg, sz);
        if (to_ea) {
            u32 d = ea_read(cpu, &e, sz);
            u32 s = cpu->d[dn] & size_mask(sz);
            u32 r = is_add ? d + s : d - s;
            ea_write(cpu, &e, sz, r);
            if (is_add) set_flags_add(cpu, sz, s, d, r);
            else        set_flags_sub(cpu, sz, s, d, r);
        } else {
            u32 s = ea_read(cpu, &e, sz);
            u32 d = cpu->d[dn] & size_mask(sz);
            u32 r = is_add ? d + s : d - s;
            cpu->d[dn] = (cpu->d[dn] & ~size_mask(sz)) | (r & size_mask(sz));
            if (is_add) set_flags_add(cpu, sz, s, d, r);
            else        set_flags_sub(cpu, sz, s, d, r);
        }
        cpu->cycles += 4;
        return;
    }

    /* ---- 1011: CMP / CMPA / CMPM / EOR ------------------------------- */
    case 0xB: {
        int dn = (op >> 9) & 7;
        int szf = (op >> 6) & 3;
        int mode = (op >> 3) & 7, reg = op & 7;
        if (szf == 3) {                         /* CMPA */
            int sz = (op & 0x0100) ? 4 : 2;
            ea_t e = ea_decode(cpu, mode, reg, sz);
            u32 s = sext(ea_read(cpu, &e, sz), sz);
            u32 d = cpu->a[dn];
            set_flags_cmp(cpu, 4, s, d, d - s);
            cpu->cycles += 6;
            return;
        }
        int sz = szf == 0 ? 1 : szf == 1 ? 2 : 4;
        bool eor_or_cmpm = (op >> 8) & 1;
        if (eor_or_cmpm) {
            if (mode == 1) {                    /* CMPM (An)+,(An)+ */
                int step = (sz == 1 && reg == 7) ? 2 : sz;
                int dstep = (sz == 1 && dn == 7) ? 2 : sz;
                u32 sa = cpu->a[reg]; cpu->a[reg] += (u32)step;
                u32 da = cpu->a[dn];  cpu->a[dn]  += (u32)dstep;
                u32 s = (sz==1)?mac_read8(cpu->mem,sa):(sz==2)?mac_read16(cpu->mem,sa):mac_read32(cpu->mem,sa);
                u32 d = (sz==1)?mac_read8(cpu->mem,da):(sz==2)?mac_read16(cpu->mem,da):mac_read32(cpu->mem,da);
                set_flags_cmp(cpu, sz, s, d, d - s);
                cpu->cycles += 12;
                return;
            }
            /* EOR Dn,<ea> */
            ea_t e = ea_decode(cpu, mode, reg, sz);
            u32 d = ea_read(cpu, &e, sz);
            u32 r = d ^ (cpu->d[dn] & size_mask(sz));
            ea_write(cpu, &e, sz, r);
            set_flags_logic(cpu, sz, r);
            cpu->cycles += 4;
            return;
        }
        /* CMP <ea>,Dn */
        ea_t e = ea_decode(cpu, mode, reg, sz);
        u32 s = ea_read(cpu, &e, sz);
        u32 d = cpu->d[dn] & size_mask(sz);
        set_flags_cmp(cpu, sz, s, d, d - s);
        cpu->cycles += 4;
        return;
    }

    /* ---- 1100: AND / MULU / MULS / EXG ------------------------------- */
    case 0xC: {
        int dn = (op >> 9) & 7;
        int szf = (op >> 6) & 3;
        int mode = (op >> 3) & 7, reg = op & 7;
        if (szf == 3) {                         /* MULU / MULS */
            bool sgn = (op >> 8) & 1;
            ea_t e = ea_decode(cpu, mode, reg, 2);
            u32 m = ea_read(cpu, &e, 2);
            u32 r;
            if (!sgn) r = (cpu->d[dn] & 0xFFFF) * (m & 0xFFFF);
            else      r = (u32)(((i32)(i16)(u16)cpu->d[dn]) * ((i32)(i16)(u16)m));
            cpu->d[dn] = r;
            set_flags_logic(cpu, 4, r);
            cpu->cycles += 70;
            return;
        }
        /* ABCD: 1100 ddd 1 0000 m rrr */
        if ((op & 0x01F0) == 0x0100) {
            int sx = op & 7, dx = (op >> 9) & 7;
            if ((op >> 3) & 1) {                /* -(Ay),-(Ax) */
                ea_t se = ea_decode(cpu, 4, sx, 1);
                u8 s = (u8)ea_read(cpu, &se, 1);
                ea_t de = ea_decode(cpu, 4, dx, 1);
                u8 d = (u8)ea_read(cpu, &de, 1);
                ea_write(cpu, &de, 1, bcd_abcd(cpu, s, d));
            } else {                            /* Dy,Dx */
                u8 r = bcd_abcd(cpu, (u8)cpu->d[sx], (u8)cpu->d[dx]);
                cpu->d[dx] = (cpu->d[dx] & ~0xFFu) | r;
            }
            cpu->cycles += 6;
            return;
        }
        /* EXG: opcode bits 8..3 select mode. */
        if ((op & 0x0130) == 0x0100) {
            int rx = (op >> 9) & 7, ry = op & 7;
            int em = (op >> 3) & 0x1F;
            if (em == 0x08) {                   /* EXG Dx,Dy */
                u32 t = cpu->d[rx]; cpu->d[rx] = cpu->d[ry]; cpu->d[ry] = t;
                return;
            } else if (em == 0x09) {            /* EXG Ax,Ay */
                u32 t = cpu->a[rx]; cpu->a[rx] = cpu->a[ry]; cpu->a[ry] = t;
                return;
            } else if (em == 0x11) {            /* EXG Dx,Ay */
                u32 t = cpu->d[rx]; cpu->d[rx] = cpu->a[ry]; cpu->a[ry] = t;
                return;
            }
        }
        int sz = szf == 0 ? 1 : szf == 1 ? 2 : 4;
        bool to_ea = (op >> 8) & 1;
        ea_t e = ea_decode(cpu, mode, reg, sz);
        if (to_ea) {
            u32 d = ea_read(cpu, &e, sz);
            u32 r = d & (cpu->d[dn] & size_mask(sz));
            ea_write(cpu, &e, sz, r);
            set_flags_logic(cpu, sz, r);
        } else {
            u32 r = (cpu->d[dn] & size_mask(sz)) & ea_read(cpu, &e, sz);
            cpu->d[dn] = (cpu->d[dn] & ~size_mask(sz)) | (r & size_mask(sz));
            set_flags_logic(cpu, sz, r);
        }
        cpu->cycles += 4;
        return;
    }

    /* ---- 1110: shifts / rotates -------------------------------------- */
    case 0xE: {
        int szf = (op >> 6) & 3;
        if (szf == 3) {
            /* Memory shift by 1. op bits 10..9 select op, bit 8 dir. */
            int mode = (op >> 3) & 7, reg = op & 7;
            int which = (op >> 9) & 3;          /* 0 ASd 1 LSd 2 ROXd 3 ROd */
            bool left = (op >> 8) & 1;
            if (!ea_is_mem(mode, reg)) { illegal(cpu, op); return; }
            ea_t e = ea_decode(cpu, mode, reg, 2);
            u32 v = ea_read(cpu, &e, 2);
            int sop = (which << 1) | (left ? 0 : 0); /* map below */
            int realop = left ? (which == 0 ? 4 : which == 1 ? 5 : which == 2 ? 6 : 7)
                              : (which == 0 ? 0 : which == 1 ? 1 : which == 2 ? 2 : 3);
            (void)sop;
            u32 r = do_shift(cpu, 2, realop, v, 1);
            ea_write(cpu, &e, 2, r);
            cpu->cycles += 8;
            return;
        }
        /* Register shift. */
        int sz = szf == 0 ? 1 : szf == 1 ? 2 : 4;
        int dn = op & 7;
        bool left = (op >> 8) & 1;
        int which = (op >> 3) & 3;              /* 0 AS 1 LS 2 ROX 3 RO */
        bool count_in_reg = (op >> 5) & 1;
        u32 cnt;
        if (count_in_reg) cnt = cpu->d[(op >> 9) & 7] & 63;
        else { cnt = (op >> 9) & 7; if (cnt == 0) cnt = 8; }
        int realop = left ? (which == 0 ? 4 : which == 1 ? 5 : which == 2 ? 6 : 7)
                          : (which == 0 ? 0 : which == 1 ? 1 : which == 2 ? 2 : 3);
        u32 v = cpu->d[dn] & size_mask(sz);
        u32 r = do_shift(cpu, sz, realop, v, cnt);
        cpu->d[dn] = (cpu->d[dn] & ~size_mask(sz)) | (r & size_mask(sz));
        cpu->cycles += 6 + 2 * (int)cnt;
        return;
    }

    /* ---- 1010: line-A emulator (Macintosh Toolbox/OS traps) ---------- */
    case 0xA:
        if (m68k_trap_hook) m68k_trap_hook(cpu, op);
        m68k_unimpl(cpu, op_pc, 10);   /* vector 10 @ 0x28 */
        return;

    /* ---- 1111: line-F emulator -------------------------------------- */
    case 0xF:
        m68k_unimpl(cpu, op_pc, 11);   /* vector 11 @ 0x2C */
        return;

    default:
        m68k_unimpl(cpu, op_pc, 4);
        return;
    }
}

/* ---- block-discovery decoder ----------------------------------------- */

/* A lightweight re-walk of just the addressing modes, to size an
 * instruction without executing it. Mirrors ea_decode's extension-word
 * consumption. Returns extra bytes beyond the opcode word. */
static u32 ea_ext_bytes(m68k_cpu *cpu, u32 pc, int mode, int reg, int sz) {
    switch (mode) {
        case 5: return 2;
        case 6: return 2;
        case 7:
            switch (reg) {
                case 0: return 2;
                case 1: return 4;
                case 2: return 2;
                case 3: return 2;
                case 4: return (sz == 4) ? 4 : 2;
                default: return 0;
            }
        default: (void)cpu; (void)pc; return 0;
    }
}

m68k_decoded m68k_decode_at(m68k_cpu *cpu, u32 pc) {
    m68k_decoded d;
    u16 op = mac_read16(cpu->mem, pc);
    d.opcode = op;
    d.length = 2;
    d.ends_block = false;
    int top = (op >> 12) & 0xF;
    int mode = (op >> 3) & 7, reg = op & 7;
    int szf = (op >> 6) & 3;
    int sz = szf == 0 ? 1 : szf == 1 ? 2 : 4;

    switch (top) {
        case 0x0: {
            int op9 = (op >> 9) & 7;
            if (((op >> 8) & 1) && mode == 1) {
                d.length += 2;                  /* MOVEP: 16-bit displacement */
            } else if (((op >> 8) & 1) || op9 == 4) {
                bool dyn = (op >> 8) & 1;
                if (!dyn) d.length += 2;        /* immediate bit number */
                d.length += ea_ext_bytes(cpu, pc, mode, reg, (mode==0)?4:1);
            } else {
                d.length += (sz == 4) ? 4 : 2;  /* immediate operand */
                /* M6.126 — ORI/ANDI/EORI #imm,CCR/SR uses mode=7/reg=4
                 * as the destination INDICATOR (not a real ea). The imm
                 * bytes are already counted above; ea_ext_bytes would
                 * over-count by 2 (mode=7/reg=4 returns 2 for sz<4 or 4
                 * for sz=4, double-fetching the imm). Special-case skip:
                 * op9 in {0=ORI, 1=ANDI, 5=EORI} with mode=7/reg=4.
                 *
                 * Same trap class as M6.122/M6.124 — mis-bounded block
                 * decodes the bytes after the real op as a phantom
                 * opcode. ORI/ANDI #imm,SR is common in interrupt-
                 * mask handlers (e.g. ORI #0x0700,SR). */
                bool to_sr_ccr = (mode == 7 && reg == 4
                                  && (op9 == 0 || op9 == 1 || op9 == 5));
                if (!to_sr_ccr) {
                    d.length += ea_ext_bytes(cpu, pc, mode, reg, sz);
                }
            }
            break;
        }
        case 0x1: case 0x2: case 0x3: {
            int msz = top == 1 ? 1 : top == 3 ? 2 : 4;
            int sm = (op >> 3) & 7, sr = op & 7;
            int dr = (op >> 9) & 7, dm = (op >> 6) & 7;
            d.length += ea_ext_bytes(cpu, pc, sm, sr, msz);
            d.length += ea_ext_bytes(cpu, pc + d.length, dm, dr, msz);
            break;
        }
        case 0x4: {
            /* Control-flow / fixed 2-byte forms first. */
            if (op == 0x4E75 || op == 0x4E73 || op == 0x4E77 || op == 0x4E70) {
                d.ends_block = true; break;
            }
            if ((op & 0xFFF0) == 0x4E40) { d.ends_block = true; break; }  /* TRAP */
            if (op == 0x4E72) { d.length += 2; d.ends_block = true; break; } /* STOP */
            if ((op & 0xFFC0) == 0x4EC0 || (op & 0xFFC0) == 0x4E80) {
                d.length += ea_ext_bytes(cpu, pc, mode, reg, 4);          /* JMP/JSR */
                d.ends_block = true; break;
            }
            if ((op & 0xFFF8) == 0x4E50) { d.length += 2; break; }        /* LINK */
            /* 2-byte instructions with NO effective-address field — these
             * must not fall through to the generic EA-extension sizing,
             * which would mis-read their register bits as an EA mode. */
            if (op == 0x4E71 || op == 0x4E76 ||                           /* NOP, TRAPV */
                (op & 0xFFF8) == 0x4840 ||                                /* SWAP */
                (op & 0xFFB8) == 0x4880 ||                                /* EXT  */
                (op & 0xFFF8) == 0x4E58 ||                                /* UNLK */
                (op & 0xFFF0) == 0x4E60) {                                /* MOVE USP */
                break;
            }
            if ((op & 0xFB80) == 0x4880) {                                /* MOVEM */
                d.length += 2 + ea_ext_bytes(cpu, pc, mode, reg, 4);
                break;
            }
            /* M6.122 — MOVE SR/CCR ops are .W regardless of szf bits.
             * Their encodings reuse the high opcode bits (bits 9-6 = 11)
             * which would normally indicate sz=long, but the immediate
             * size for MOVE-to-SR / MOVE-to-CCR is ALWAYS 16-bit (word).
             * Without this, m68k_decode_at returned length 6 for 0x46FC
             * (MOVE #imm,SR) instead of 4, causing the JIT block compiler
             * to walk past the imm word and decode the BYTE AFTER as a
             * new opcode. The phantom op fell to the default helper at
             * compile time; the helper bridge's emit_advance_flush set
             * cpu->pc to where the *real* M6.117 MOVE-to-SR inline left
             * it, and m68k_step then decoded the REAL next instruction
             * — accidentally correct runtime behavior. But ANY inline
             * arm for the phantom op's opcode would have corrupted
             * execution. Bench's 0x4A38 21 K helpers were a side effect
             * of this trap (the "phantom" PC=0x4010E0 m68k_step calls).
             *
             *   MOVE SR,<ea>   = 0x40C0-0x40FF  (bits 15-6 = 0x103)
             *   MOVE CCR,<ea>  = 0x42C0-0x42FF  (bits 15-6 = 0x10B)
             *   MOVE <ea>,CCR  = 0x44C0-0x44FF  (bits 15-6 = 0x113)
             *   MOVE <ea>,SR   = 0x46C0-0x46FF  (bits 15-6 = 0x11B)
             *
             * All are word-size; sz here should be 2. */
            int ea_sz = sz == 0 ? 2 : sz;
            /* (op & 0xF1C0) == 0x40C0 covers all four MOVE SR/CCR forms
             * (bits 15-12 = 0100, bit 11 = 0, bit 8 = 0, bits 7-6 = 11).
             * Bits 11-9 distinguish: 000=MOVE-from-SR, 001=MOVE-from-CCR,
             * 010=MOVE-to-CCR, 011=MOVE-to-SR. */
            if ((op & 0xF1C0) == 0x40C0) ea_sz = 2;
            /* M6.124b — CHK.W <ea>,Dn (op = 0100 ddd 110 mmm rrr =
             * 0x41C0-base with bits 8-6 = 110, i.e. (op & 0xF1C0) == 0x4180).
             * On 68000 CHK is always .W, but the szf bits 7-6 = 10 map to
             * sz=4 in the generic decoder. For CHK.W #imm,Dn the imm is
             * 2 bytes (.W), not 4. Same trap class as the MOVE-SR fix:
             * mis-bounded block decodes the next 2 bytes as a phantom
             * opcode. CHK is rare so this hasn't shown up in bench/boot
             * helper-histo, but fix for forward robustness. */
            if ((op & 0xF1C0) == 0x4180) ea_sz = 2;
            d.length += ea_ext_bytes(cpu, pc, mode, reg, ea_sz);
            break;
        }
        case 0x5: {
            if (szf == 3 && mode == 1) { d.length += 2; d.ends_block = true; break; } /* DBcc */
            d.length += ea_ext_bytes(cpu, pc, mode, reg, sz);
            break;
        }
        case 0x6: {
            if ((op & 0xFF) == 0x00) d.length += 2;
            else if ((op & 0xFF) == 0xFF) d.length += 4;
            d.ends_block = true;            /* Bcc/BRA/BSR all end the block */
            break;
        }
        case 0x7: break;                    /* MOVEQ */
        case 0x8: case 0x9: case 0xB: case 0xC: case 0xD: {
            /* M6.124 — szf=3 disambiguates by top:
             *   top=0x8/0xC : DIVU/DIVS/MULU/MULS — always .W operand (sz=2)
             *   top=0x9/0xB/0xD : ADDA/SUBA/CMPA — sz depends on bit 8
             *     bit 8 = 0 → .W (sz=2); bit 8 = 1 → .L (sz=4)
             *
             * Without the bit-8 distinction for top=9/B/D, ADDA.L/SUBA.L/
             * CMPA.L with #imm32 source would have decoder return length 4
             * (op + 2 byte imm) instead of 6 (op + 4 byte imm). The JIT
             * block compiler would walk past only the first 2 bytes of
             * the imm32 and decode the next 2 bytes as a phantom opcode —
             * the same trap class as M6.122 (MOVE-to-SR length bug).
             *
             * The phantom emission is accidentally correct for most paths
             * because default-helper bridges' m68k_step decodes from
             * runtime cpu->pc (advanced by the actual op length via the
             * step's internal fetch), not from the wrong compile-time
             * op_pc. But any new inline arm for the phantom's opcode
             * would corrupt execution. */
            int adj_sz;
            if (szf == 3) {
                bool a_long = (top == 0x9 || top == 0xB || top == 0xD)
                              && (op & 0x0100);
                adj_sz = a_long ? 4 : 2;
            } else {
                adj_sz = sz;
            }
            d.length += ea_ext_bytes(cpu, pc, mode, reg, adj_sz);
            break;
        }
        case 0xE: {
            if (szf == 3) d.length += ea_ext_bytes(cpu, pc, mode, reg, 2);
            break;
        }
        /* line-A (Toolbox trap) and line-F take an exception — control
         * leaves the block, so they terminate it. */
        case 0xA: case 0xF:
            d.ends_block = true;
            break;
        default: break;
    }
    if (d.length < 2) d.length = 2;
    return d;
}

/* ---- run loop --------------------------------------------------------- */

void m68k_run_until(m68k_cpu *cpu, u64 until) {
    while (cpu->cycles < until && !cpu->halted) {
        mac_mem_tick(cpu->mem, cpu->cycles);
        if (m68k_poll_interrupts(cpu)) continue;
        if (sony_service(cpu)) continue;
        if (cpu->stopped) {
            /* Idle until the next peripheral edge raises an interrupt.
             * The increment matches the JIT dispatcher's idle step so a
             * STOP-terminated program ends at the same cycle count under
             * both engines. */
            cpu->cycles += 64;
            continue;
        }
        m68k_step(cpu);
    }
}
