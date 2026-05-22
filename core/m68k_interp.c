/* Motorola 68000 reference interpreter.
 *
 * Covers the bulk of the user- and supervisor-mode integer ISA: every
 * effective-addressing mode, MOVE/MOVEA/MOVEQ, the immediate ALU group,
 * ADD/SUB/AND/OR/EOR/CMP in all forms, ADDA/SUBA/CMPA, ADDQ/SUBQ, the
 * shift/rotate family, Bcc/BRA/BSR/DBcc/Scc, JMP/JSR/RTS/RTR/RTE, the
 * bit ops, MULU/MULS/DIVU/DIVS, MOVEM, LEA/PEA, EXT/SWAP/LINK/UNLK,
 * NEG/NEGX/NOT/CLR/TST/TAS, TRAP/exceptions and autovector interrupts.
 *
 * Not modelled: BCD (ABCD/SBCD/NBCD), MOVEP, the 68010+ additions. Those
 * decode to an illegal-instruction exception.
 *
 * Cycle counts are approximate (good enough to pace the ~60 Hz VBL); the
 * goal is functional correctness, which the JIT differential test pins. */

#include "m68k_interp.h"
#include "mac_mem.h"
#include "sony.h"
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

void m68k_step(m68k_cpu *cpu) {
    /* Once the CPU has halted (e.g. the guest wrote the debug exit port),
     * further steps are no-ops. This keeps a JIT block — which may contain
     * instructions after the halting one — from diverging from the
     * interpreter's run loop, which stops the instant `halted` is set. */
    if (cpu->halted) return;
    u32 op_pc = cpu->pc;
    u16 op = fetch16(cpu);
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
        if (op == 0x4E70) { cpu->halted = M68K_HALT_RESET;    return; } /* RESET -> stop */
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
        if ((op & 0xFF00) == 0x4800 && ((op >> 6) & 3) != 3 && (op & 0x0040) == 0 && ((op>>6)&3)==0) {
            /* NBCD — not modelled. */
            illegal(cpu, op); return;
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
                    r = (u32)(0 - d - (X ? 1 : 0));
                    ea_write(cpu, &e, sz, r);
                    u8 c = 0;
                    u32 mask = size_mask(sz), msb = size_msb(sz);
                    if ((r & mask) & msb) c |= CCR_N;
                    if ((r & mask) != 0 && (m68k_get_ccr(cpu) & CCR_Z))
                        c |= CCR_Z;
                    else if ((r & mask) == 0 && (m68k_get_ccr(cpu) & CCR_Z))
                        c |= CCR_Z;
                    if (d != 0 || X) c |= CCR_C | CCR_X;
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
            if (is_add) { r = d + s + (X?1:0); set_flags_add(cpu, sz, s, d, r); }
            else        { r = d - s - (X?1:0); set_flags_sub(cpu, sz, s, d, r); }
            /* X-form leaves Z sticky: only clears on nonzero, never sets. */
            if ((r & size_mask(sz)) != 0)
                m68k_set_ccr(cpu, (u8)(m68k_get_ccr(cpu) & ~CCR_Z));
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
                d.length += ea_ext_bytes(cpu, pc, mode, reg, sz);
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
            /* Remaining case-4 forms all carry a normal EA: NEG/NEGX/CLR/
             * NOT/TST/TAS, MOVE SR/CCR, PEA, LEA, CHK. */
            d.length += ea_ext_bytes(cpu, pc, mode, reg, sz == 0 ? 2 : sz);
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
            d.length += ea_ext_bytes(cpu, pc, mode, reg, (szf==3)?2:sz);
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
