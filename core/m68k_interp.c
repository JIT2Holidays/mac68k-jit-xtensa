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
        /* USP is undefined at hardware reset on the 68030. On the Mac
         * SE/30 the ROM does `MOVEC USP, D7` early in boot and inspects
         * bit 16 to decide between normal boot and diagnostic mode.
         * Real cold-boot USP can be any value; setting bit 16 here
         * (only under SE/30) takes the normal-boot path. Mac Plus path
         * leaves USP = 0 to keep lockstep snapshots bit-identical.
         * TODO(se30-usp): determine if there's a real hardware power-on
         * convention or if this is meant to be set by a sense-line. */
        if (mem->model == MAC_MODEL_SE30) {
            /* M7.6bd — Per vmac comparison (M7.6bc), USP=0x00010000
             * (bit 16 set) was wrong: vmac's D7=0x00000004 after
             * diagnostic-mode entry, meaning the ROM's
             * MOVEC USP,D7 path does NOT have bit 16 set. Setting USP
             * to 0 matches vmac's behavior — ROM takes the diagnostic
             * path naturally and (with right hardware) proceeds to
             * boot. */
            cpu->usp = 0;
            /* M7.6x — canonical "MMU disabled" SRP/CRP per minivmac's
             * Mac IIx reference (third_party/minivmac MINEM68K.c
             * DoCodeMMU): PMOVE SRP/CRP, (A0) emits 0x7FFF0001 then
             * 0x00000000 — LIMIT=0x7FFF (max), DT=1 invalid table.
             * The boot ROM stores SRP/CRP to memory during init and
             * compares to this canonical pattern; returning zeros
             * confused some of the hardware-detect paths. */
            cpu->srp = ((u64)0x7FFF0001u << 32);
            cpu->crp = cpu->srp;
        }
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

/* Resolve the index displacement used by modes (d8,An,Xn) and
 * (d8,PC,Xn). On the 68000 only the brief format (bit 8 = 0) is legal;
 * on the 68020+ bit 8 = 1 selects the full extension format, which adds
 * a scaled index, an optional base displacement (word or long) and
 * memory-indirect modes with an outer displacement. */
static u32 brief_index(m68k_cpu *cpu, u32 base) {
    u16 ext = fetch16(cpu);
    int ireg = (ext >> 12) & 7;
    u32 ival = (ext & 0x8000) ? cpu->a[ireg] : cpu->d[ireg];
    if (!(ext & 0x0800)) ival = (u32)(i32)(i16)(u16)ival;   /* word index */
    if (!(ext & 0x0100)) {
        /* Brief format — single signed-byte displacement. */
        i32 disp = (i8)(ext & 0xFF);
        return base + ival + (u32)disp;
    }
    /* Full format (68020+). */
    int scale = (ext >> 9) & 3;       /* 0..3 → ×1,×2,×4,×8 */
    bool bs = (ext & 0x80) != 0;      /* base suppress */
    bool is = (ext & 0x40) != 0;      /* index suppress */
    int bd_size = (ext >> 4) & 3;     /* 0 reserved, 1 null, 2 word, 3 long */
    int iis = ext & 7;                /* indirect / outer disp selector */
    u32 bd = 0;
    if (bd_size == 2) bd = (u32)(i32)(i16)fetch16(cpu);
    else if (bd_size == 3) bd = fetch32(cpu);
    u32 base_eff = bs ? 0u : base;
    u32 idx_eff = is ? 0u : (ival << scale);
    u32 intermediate = base_eff + bd;
    if (iis == 0) {
        /* No memory indirection. */
        return intermediate + idx_eff;
    }
    /* Memory-indirect: pre-indexed if iis in 1..3 (and IS clear), or
     * post-indexed if iis in 5..7. With IS set, all 1..3 are plain
     * memory-indirect (index has no effect since it's suppressed). */
    bool postindexed = (iis >= 5);
    int od_sel = iis & 3;             /* 0/1/2/3 — null/null/word/long */
    u32 od = 0;
    if (od_sel == 2) od = (u32)(i32)(i16)fetch16(cpu);
    else if (od_sel == 3) od = fetch32(cpu);
    u32 addr = postindexed ? intermediate : intermediate + idx_eff;
    if (cpu->mem) addr = mac_read32(cpu->mem, addr);
    return addr + (postindexed ? idx_eff : 0u) + od;
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

    bool is_030 = (cpu->mem && cpu->mem->model == MAC_MODEL_SE30);
    if (is_030 && vector == 2) {
        /* M7.6r — Format-A short bus error stack frame (16 words = 32 bytes)
         * per 68030 manual Figure 6-6. Only the SR / PC / format-vector /
         * fault address fields are populated; the SSW + pipe + cycle-info
         * fields stay zero. The SE/30 ROM's BERR handler reads the format
         * word at SP+6 to decide frame size, and the fault address at
         * SP+0x0E to know which probe failed. */
        cpu->a[7] -= 32;
        u32 sp = cpu->a[7];
        mac_write16(cpu->mem, sp + 0x00, saved_sr);
        mac_write32(cpu->mem, sp + 0x02, cpu->pc);
        mac_write16(cpu->mem, sp + 0x06, (u16)(0xA000u | (vector * 4u)));
        mac_write16(cpu->mem, sp + 0x08, 0);                /* SSW */
        mac_write16(cpu->mem, sp + 0x0A, 0);                /* pipe C */
        mac_write16(cpu->mem, sp + 0x0C, 0);                /* pipe B */
        mac_write32(cpu->mem, sp + 0x0E, cpu->fault_addr);  /* fault LA */
        mac_write32(cpu->mem, sp + 0x12, 0);                /* cycle 1 IB */
        mac_write32(cpu->mem, sp + 0x16, 0);                /* cycle 2 IB */
        mac_write32(cpu->mem, sp + 0x1A, 0);                /* cycle 3 IB */
    } else if (is_030) {
        /* Format-0 short frame: SR / PC / format-vector (8 bytes total). */
        cpu->a[7] -= 8;
        u32 sp = cpu->a[7];
        mac_write16(cpu->mem, sp + 0, saved_sr);
        mac_write32(cpu->mem, sp + 2, cpu->pc);
        mac_write16(cpu->mem, sp + 6, (u16)(vector * 4u));
    } else {
        /* Plus / 68000 short frame: SR + PC (6 bytes), no format word. */
        cpu->a[7] -= 4;
        mac_write32(cpu->mem, cpu->a[7], cpu->pc);
        cpu->a[7] -= 2;
        mac_write16(cpu->mem, cpu->a[7], saved_sr);
    }
    /* 68010+ vector table is relocatable via VBR; for the 68000 (Plus mode)
     * VBR is always 0, so this is bit-for-bit identical to the historical
     * `mac_read32(cpu->mem, vector * 4u)`. */
    cpu->pc = mac_read32(cpu->mem, cpu->vbr + vector * 4u);
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

    u32 vector = 24u + level;
    if (cpu->mem && cpu->mem->model == MAC_MODEL_SE30) {
        /* M7.6r — Format-0 8-byte short frame for interrupts on 030. */
        cpu->a[7] -= 8;
        u32 sp = cpu->a[7];
        mac_write16(cpu->mem, sp + 0, saved_sr);
        mac_write32(cpu->mem, sp + 2, cpu->pc);
        mac_write16(cpu->mem, sp + 6, (u16)(vector * 4u));
    } else {
        cpu->a[7] -= 4;
        mac_write32(cpu->mem, cpu->a[7], cpu->pc);
        cpu->a[7] -= 2;
        mac_write16(cpu->mem, cpu->a[7], saved_sr);
    }
    /* Autovector: level n -> vector 24+n. Relocated through VBR on 68010+. */
    cpu->pc = mac_read32(cpu->mem, cpu->vbr + vector * 4u);
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

/* M6.144 — MOVE.L (An),Dn MMIO fast helper. Replaces the m68k_step
 * bridge in M6.127's slow path. Bench's 0x2014 (MOVE.L (A4),D0) at
 * 156 hits/100M cyc all target MMIO. Pattern (top=2, bits 8-6=0,
 * mode=2) is STRICTLY ABSENT from boot 100M per the trajectory-safety
 * scan.
 *
 * Args: jit_arg1 = src addr (= cpu->a[an]); jit_arg2 = dn (0..7). */
void m68k_jit_move_l_an_to_dn_mmio(m68k_cpu *cpu) {
    int dn = (int)(cpu->jit_arg2 & 7);
    u32 addr = cpu->jit_arg1;
    u32 v = mac_read32(cpu->mem, addr);
    cpu->d[dn] = v;
    u8 ccr = m68k_get_ccr(cpu) & CCR_X;
    if (v == 0)             ccr |= CCR_Z;
    if (v & 0x80000000u)    ccr |= CCR_N;
    m68k_set_ccr(cpu, ccr);
}

/* M6.161 — CLR.W (An)+ MMIO fast helper. Slow-path conversion of
 * M6.130's CLR.W (An)+ inline arm (which previously bridged to
 * m68k_step for the MMIO case). Boot 100M's 0x4258 fires 309 times
 * (real-code pc=0x4087be in ROM trap dispatcher) — the highest-fire
 * remaining boot helper that's a slow-path conversion candidate.
 *
 * Args: jit_arg2 = an (0..7). jit_arg1 unused.
 * Semantics: mem.W[An] = 0, An += 2, CCR = X-preserved | Z. */
void m68k_jit_clr_w_anpi_mmio(m68k_cpu *cpu) {
    int an = (int)(cpu->jit_arg2 & 7);
    u32 addr = cpu->a[an];
    mac_write16(cpu->mem, addr, 0);
    cpu->a[an] = addr + 2;
    u8 ccr = m68k_get_ccr(cpu) & CCR_X;
    ccr |= CCR_Z;
    m68k_set_ccr(cpu, ccr);
}

/* M6.169 — TST.B (d16,An) MMIO fast helper. thinkc8-folder-open bench's
 * 638 K hits / 100 M cyc on the Finder linked-list walk at PC=0x3E580E.
 * The JIT arm pre-computes EA = An + (s16)d16 and passes it via
 * jit_arg1; this helper just reads the byte (mac_read8 dispatches to
 * RAM, ROM, or MMIO transparently) and sets N/Z. V/C cleared, X kept.
 * No D/A reg writes — JIT can skip emit_cache_reload after the bridge. */
void m68k_jit_tst_b_mmio(m68k_cpu *cpu) {
    u32 addr = cpu->jit_arg1;
    u8 d = mac_read8(cpu->mem, addr);
    u8 ccr = m68k_get_ccr(cpu) & CCR_X;
    if (d == 0)   ccr |= CCR_Z;
    if (d & 0x80) ccr |= CCR_N;
    m68k_set_ccr(cpu, ccr);
}

/* M6.198 — RTE fast helper. Boot 100M's 0x4E73 at PC=0x401A82 fires
 * 390 times — exception return. Skips m68k_step's decode + cpu->instrs
 * increment.
 *
 * Semantics (per m68k_interp.c:1043):
 *   bool was = m68k_is_super(cpu);
 *   u16 sr = mac_read16(a[7]);    cpu->a[7] += 2;
 *   u32 pc = mac_read32(a[7]);    cpu->a[7] += 4;
 *   cpu->sr = sr; cpu->pc = pc;
 *   m68k_sync_sp(cpu, was);
 *
 * No args. Block terminator (helper sets cpu->pc). */
void m68k_jit_rte(m68k_cpu *cpu) {
    bool was = m68k_is_super(cpu);
    u32 a7 = cpu->a[7];
    u16 sr = mac_read16(cpu->mem, a7);
    u32 pc = mac_read32(cpu->mem, a7 + 2);
    /* M7.6r — pop frame size based on format word (030 only). */
    u32 frame_size = 6;
    if (cpu->mem && cpu->mem->model == MAC_MODEL_SE30) {
        u16 fmt_vec = mac_read16(cpu->mem, a7 + 6);
        u32 fmt = (fmt_vec >> 12) & 0xFu;
        frame_size = (fmt == 0xA) ? 32u : 8u;
    }
    cpu->a[7] = a7 + frame_size;
    cpu->sr = sr;
    cpu->pc = pc;
    m68k_sync_sp(cpu, was);
}

/* M6.204 — BTST/BCHG/BCLR/BSET Dn,(An) MMIO fast helper. Slow-path
 * conversion for M6.202's inline arm: when An lands outside RAM
 * (MMIO range, etc.) the bridge used to call m68k_step which decodes
 * the opcode from scratch and increments cpu->instrs. This helper
 * skips both — saves ~30 LX7 per fire and drops real_helpers by the
 * same count.
 *
 * Args:
 *   jit_arg1 = guest EA address (already runtime-computed in the arm
 *              from An, before the bounds check).
 *   jit_arg2 = packed (which << 4) | dn:
 *              which: 0=BTST, 1=BCHG, 2=BCLR, 3=BSET
 *              dn:    source Dn register (0..7) for bit number
 *
 * Mirrors do_bitop()'s memory path (mode != 0, sz=1): bit number is
 * Dn & 7; sets only Z; for non-BTST modifies the byte. PC/cycle
 * advance is handled by the JIT arm's emit_advance — the helper
 * touches neither cpu->pc nor cpu->cycles.
 *
 * Boot 100M's 0x09D1 (BSET D4,(A1)) at PC=0x40025E fires 696 times
 * with A1 → MMIO. Boot-cycle100m fires 688, boot-rom-init fires 688.
 * Per the [[slow-path-conversion-threshold]] rule (≥150 fires safe),
 * 696 is well above the M6.162 threshold. */
void m68k_jit_bitop_dn_an_mmio(m68k_cpu *cpu) {
    u32 addr   = cpu->jit_arg1;
    u32 packed = cpu->jit_arg2;
    int which  = (int)((packed >> 4) & 3);
    int dn     = (int)(packed & 7);
    int bit    = (int)(cpu->d[dn] & 7);
    u8 v       = mac_read8(cpu->mem, addr);
    u8 mask    = (u8)(1u << bit);
    u8 ccr     = m68k_get_ccr(cpu) & (u8)~CCR_Z;
    if (!(v & mask)) ccr |= CCR_Z;
    m68k_set_ccr(cpu, ccr);
    switch (which) {
        case 0: break;                                       /* BTST */
        case 1: mac_write8(cpu->mem, addr, (u8)(v ^ mask)); break;  /* BCHG */
        case 2: mac_write8(cpu->mem, addr, (u8)(v & (u8)~mask)); break; /* BCLR */
        case 3: mac_write8(cpu->mem, addr, (u8)(v | mask)); break;  /* BSET */
    }
}

/* M6.243 — MOVEA.W (addr),Am MMIO fast helper. Sibling of M6.240d for
 * the .W variant: sign-extends .W to 32-bit before writing to An.
 * thinkc-bullseye fires 0x366E + 0x346E ~37K combined via MMIO bridge.
 *
 * Args:
 *   jit_arg1 = src addr (caller-computed)
 *   jit_arg2 = dst Am idx (0..7)
 *
 * Reads .W from src, sign-extends to 32, writes to cpu->a[am].
 * MOVEA never touches CCR. */
void m68k_jit_movea_w_addr_to_am_mmio(m68k_cpu *cpu) {
    int am = (int)(cpu->jit_arg2 & 7);
    u32 addr = cpu->jit_arg1;
    u16 v = mac_read16(cpu->mem, addr);
    cpu->a[am] = (u32)(i32)(i16)v;
    /* MOVEA — no CCR. */
}

/* M6.242 — MOVE.W (addr),(Am)+ MMIO fast helper. Used by the MOVE.W
 * (d16,An),(Am)+ inline arm when src and/or dst resolve to MMIO.
 * thinkc-bullseye fires this pattern heavily (0x32E9 + 0x366E + 0x346E
 * ~63K combined).
 *
 * Args:
 *   jit_arg1 = src addr (caller-computed)
 *   jit_arg2 = dst Am idx (0..7)
 *
 * Reads .W from src, writes to cpu->a[am], post-increments An by 2.
 * MOVE-family CCR (N from bit 15, Z from .W == 0, V/C=0, X preserved). */
void m68k_jit_move_w_addr_to_postinc_mmio(m68k_cpu *cpu) {
    int am = (int)(cpu->jit_arg2 & 7);
    u32 src = cpu->jit_arg1;
    u16 v = mac_read16(cpu->mem, src);
    u32 dst = cpu->a[am];
    mac_write16(cpu->mem, dst, v);
    /* .W postinc: A7 increments by 2 too (same as size, only .B differs). */
    cpu->a[am] = dst + 2;
    u8 ccr = m68k_get_ccr(cpu) & CCR_X;
    if (v == 0)     ccr |= CCR_Z;
    if (v & 0x8000) ccr |= CCR_N;
    m68k_set_ccr(cpu, ccr);
}

/* M6.241 — CMP.W (addr),Dn MMIO fast helper. Used by CMP.W (d16,An),Dn /
 * CMP.W (An),Dn / CMP.W (d8,An,Xn),Dn arms when source address resolves
 * to MMIO. thinkc-bullseye fires CMP.W (d16,An),Dn family (0xB66E, 0xB06E,
 * 0xB46E) ~52K combined.
 *
 * Args:
 *   jit_arg1 = src addr (caller-computed)
 *   jit_arg2 = Dn (0..7)
 *
 * Reads .W from src, compares against Dn.W; sets N/Z/V/C, preserves X.
 * Mirrors set_flags_cmp() for sz=2 (.W). Skips m68k_step's decode +
 * cpu->instrs++. */
void m68k_jit_cmp_w_addr_dn_mmio(m68k_cpu *cpu) {
    int dn = (int)(cpu->jit_arg2 & 7);
    u32 addr = cpu->jit_arg1;
    u32 s = mac_read16(cpu->mem, addr);          /* zero-extended */
    u32 d = cpu->d[dn] & 0xFFFFu;
    u32 r = (d - s) & 0xFFFFu;
    u8 c = m68k_get_ccr(cpu) & CCR_X;
    if (r == 0)               c |= CCR_Z;
    if (r & 0x8000u)          c |= CCR_N;
    if (s > d)                c |= CCR_C;
    if (((s ^ d) & (d ^ r)) & 0x8000u) c |= CCR_V;
    m68k_set_ccr(cpu, c);
}

/* M6.240d — MOVEA.L src,Am MMIO fast helper. Generic addr→An.L
 * variant. Used by MOVEA.L (d16,An),Am / MOVEA.L (An),Am /
 * MOVEA.L (xxx).W,Am / MOVEA.L (d8,An,Xn),Am arms when source addr
 * resolves to MMIO. thinkc-bullseye fires (d16,An),Am variants
 * ~118K combined across 0x206E/0x2A6E/0x286E.
 *
 * Args:
 *   jit_arg1 = src addr
 *   jit_arg2 = dst Am (0..7)
 *
 * Reads .L (4 BE bytes), writes to cpu->a[am]. MOVEA writes full
 * 32 bits (no sign-extend for .L). MOVEA never touches CCR.
 * Skips m68k_step's decode + cpu->instrs++. */
void m68k_jit_movea_l_addr_to_am_mmio(m68k_cpu *cpu) {
    int am = (int)(cpu->jit_arg2 & 7);
    u32 addr = cpu->jit_arg1;
    u32 v = mac_read32(cpu->mem, addr);
    cpu->a[am] = v;
    /* MOVEA — no CCR. */
}

/* M6.240 — MOVE.B (An)+,Dn MMIO fast helper. Sibling of M6.132's
 * m68k_jit_move_l_postinc_to_dn_mmio for the .B variant. Used by the
 * MOVE.B (An)+,Dn arm (M6.94) when An resolves to MMIO. thinkc-bullseye
 * fires 0x1018/0x1019 family ~132K combined.
 *
 * Args: jit_arg2 packed = dn | (an << 4). Reads cpu->a[an] for addr.
 * Reads 1 byte, merges into low 8 of cpu->d[dn], post-increments An by
 * 1 (or 2 for A7). MOVE-family flags. */
void m68k_jit_move_b_postinc_to_dn_mmio(m68k_cpu *cpu) {
    int dn = (int)(cpu->jit_arg2 & 7);
    int an = (int)((cpu->jit_arg2 >> 4) & 7);
    u32 addr = cpu->a[an];
    u8 v = mac_read8(cpu->mem, addr);
    int step = (an == 7) ? 2 : 1;
    cpu->a[an] = addr + (u32)step;
    cpu->d[dn] = (cpu->d[dn] & 0xFFFFFF00u) | v;
    u8 ccr = m68k_get_ccr(cpu) & CCR_X;
    if (v == 0)   ccr |= CCR_Z;
    if (v & 0x80) ccr |= CCR_N;
    m68k_set_ccr(cpu, ccr);
}

/* M6.239 — MOVE.W src,Dn MMIO fast helper. Sibling of M6.133's
 * m68k_jit_move_b_addr_to_dn_mmio for the .W variant. Used by the
 * MOVE.W (d16,An),Dn / MOVE.W (An),Dn / MOVE.W (xxx).W,Dn arms when
 * the source address resolves to MMIO. thinkc-bullseye fires the
 * (d16,An) variant 96K times.
 *
 * Args:
 *   jit_arg1 = src addr (caller-computed)
 *   jit_arg2 = dst Dn (0..7)
 *
 * Reads .W (2 BE bytes), merges into low 16 of cpu->d[dn] preserving
 * high 16. MOVE-family CCR (N from bit 15, Z from .W==0, V=C=0,
 * X preserved). Skips m68k_step's decode + cpu->instrs++. */
void m68k_jit_move_w_addr_to_dn_mmio(m68k_cpu *cpu) {
    int dn = (int)(cpu->jit_arg2 & 7);
    u32 addr = cpu->jit_arg1;
    u16 v = mac_read16(cpu->mem, addr);
    cpu->d[dn] = (cpu->d[dn] & 0xFFFF0000u) | v;
    u8 ccr = m68k_get_ccr(cpu) & CCR_X;
    if (v == 0)     ccr |= CCR_Z;
    if (v & 0x8000) ccr |= CCR_N;
    m68k_set_ccr(cpu, ccr);
}

/* M6.229 — MOVE.L src,dst mem-to-mem MMIO fast helper, .L size.
 * Sibling of [[m68k_jit_move_b_addr_to_addr_mmio]] for boot-system-load's
 * 0x2F72 (MOVE.L (d8,A2,Xn),(d16,A7)) at PC=0x401F74 — 229,358 fires.
 *
 * Args:
 *   jit_arg1 = src addr (compile-time-decoded, runtime-computed)
 *   jit_arg2 = dst addr (compile-time-decoded, runtime-computed)
 *
 * Reads 4 BE bytes from src, writes to dst. MOVE-family CCR (N/Z
 * from .L value, V=C=0, X preserved). */
void m68k_jit_move_l_addr_to_addr_mmio(m68k_cpu *cpu) {
    u32 src = cpu->jit_arg1;
    u32 dst = cpu->jit_arg2;
    u32 v = mac_read32(cpu->mem, src);
    mac_write32(cpu->mem, dst, v);
    u8 ccr = m68k_get_ccr(cpu) & CCR_X;
    if (v == 0)          ccr |= CCR_Z;
    if (v & 0x80000000u) ccr |= CCR_N;
    m68k_set_ccr(cpu, ccr);
}

/* M6.228 — MOVE.B (d16,An),(d16,Am) mem-to-mem MMIO fast helper.
 * boot-system-load 0x1F6B (MOVE.B (d16,A3),(d16,A7)) fires 229,358 times
 * via default m68k_step bridge — src EA resolves to MMIO each time.
 *
 * Both args are RUNTIME-computed in the inline arm (not compile-time
 * imm), so the bridge stores both via xt_s32i instead of going through
 * emit_jit_fast_helper (which only handles a8+imm).
 *
 * Args:
 *   jit_arg1 = src addr (a[src_an] + d16_src)
 *   jit_arg2 = dst addr (a[dst_an] + d16_dst)
 *
 * Reads 1 BE byte from src, writes to dst. MOVE-family CCR (N/Z from
 * byte, V=C=0, X preserved). */
void m68k_jit_move_b_addr_to_addr_mmio(m68k_cpu *cpu) {
    u32 src = cpu->jit_arg1;
    u32 dst = cpu->jit_arg2;
    u8 v = mac_read8(cpu->mem, src);
    mac_write8(cpu->mem, dst, v);
    u8 ccr = m68k_get_ccr(cpu) & CCR_X;
    if (v == 0)   ccr |= CCR_Z;
    if (v & 0x80) ccr |= CCR_N;
    m68k_set_ccr(cpu, ccr);
}

/* M6.225 — MOVE.L (xxx).W,(An) MMIO fast helper. boot-system-load
 * fires 0x22B8 (MOVE.L (xxx).W,(A1)) at PC=0xA001B6?? 114,679 times
 * where default helper bridges to m68k_step.
 *
 * Args:
 *   jit_arg1 = src abs addr (compile-time-known imm.W, sign-extended)
 *   jit_arg2 = dst An register index (0..7)
 *
 * Reads 4 BE bytes from abs, writes to (a[an]). Sets MOVE-family CCR
 * (N/Z from value, V=C=0, X preserved). */
void m68k_jit_move_l_xxxw_to_an_mmio(m68k_cpu *cpu) {
    u32 src_addr = cpu->jit_arg1;
    int an = (int)(cpu->jit_arg2 & 7);
    u32 v = mac_read32(cpu->mem, src_addr);
    mac_write32(cpu->mem, cpu->a[an], v);
    u8 ccr = m68k_get_ccr(cpu) & CCR_X;
    if (v == 0)          ccr |= CCR_Z;
    if (v & 0x80000000u) ccr |= CCR_N;
    m68k_set_ccr(cpu, ccr);
}

/* M6.193 — MOVE (An)+,SR fast helper. thinkc8-folder-open bench's
 * 0x46DF (MOVE (A7)+,SR) at PC=0x4027E0 ~25K hits/100 M (critical-
 * section SR-restore pattern). Skips m68k_step's decode + cpu->instrs
 * increment.
 *
 * Args: jit_arg2 = An register index (0..7); jit_arg1 unused.
 * The full m68k_sync_sp is called so S-bit changes are handled
 * correctly. */
void m68k_jit_move_anpi_to_sr(m68k_cpu *cpu) {
    int an = (int)(cpu->jit_arg2 & 7);
    u32 addr = cpu->a[an];
    u16 val = mac_read16(cpu->mem, addr);
    cpu->a[an] = addr + 2;
    bool was = m68k_is_super(cpu);
    cpu->sr = val;
    m68k_sync_sp(cpu, was);
}

/* M6.190 — A-line trap fast helper. thinkc8-folder-open bench's
 * 0xA000-0xAFFF range fires ~25 K times/100 M cyc (Toolbox trap
 * dispatch). Sibling of m68k_jit_fline_trap below — just calls
 * m68k_exception(cpu, 10).
 *
 * Note: skips the m68k_trap_hook call. Trace mode (MAC68K_TRACE_FROM/
 * _TO) won't see line-A traces under --jit; use --interp if needed. */
void m68k_jit_aline_trap(m68k_cpu *cpu) {
    m68k_exception(cpu, 10);
}

/* M6.137 — F-line trap fast helper. Bench-hot 0xFFFF at 21 808 hits /
 * 100 M cyc (the bench's M6.66-equivalent divergence zone fetches
 * 0xFFFF from unmapped memory, which top-nibble F decodes to line-F).
 * Replaces m68k_step's F-line dispatch path:
 *   - cpu->instrs NOT incremented (real_helpers drops 21 K for bench)
 *   - cpu->pc set by JIT's emit_advance_flush to op_pc; m68k_exception
 *     pushes that, then sets cpu->pc to vector 11's value
 *   - m68k_exception itself adds 34 cycles; JIT's emit_advance(0, 4)
 *     adds the m68k_step base 4 to total 38 (same as m68k_step path) */
void m68k_jit_fline_trap(m68k_cpu *cpu) {
    /* cpu->pc was set by emit_advance_flush to the faulting op_pc. */
    m68k_exception(cpu, 11);
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

void m68k_jit_move_b_imm_to_addr_mmio(m68k_cpu *cpu) {
    /* MOVE.B #imm,addr — writes compile-time-known imm byte to addr.
     * jit_arg1 = addr, jit_arg2 = imm.B (low 8 bits). */
    u32 addr = cpu->jit_arg1;
    u8 v = (u8)(cpu->jit_arg2 & 0xFF);
    mac_write8(cpu->mem, addr, v);
    u8 ccr = m68k_get_ccr(cpu) & CCR_X;
    if (v == 0)        ccr |= CCR_Z;
    if (v & 0x80)      ccr |= CCR_N;
    m68k_set_ccr(cpu, ccr);
}

void m68k_jit_move_b_addr_to_an_mmio(m68k_cpu *cpu) {
    /* MOVE.B src_addr→(Am) — mem-to-mem byte copy. Used by the M6.91
     * MOVE.B (d16,An),(Am) arm. Boot's 0x10A8 (MOVE.B (d16,A0),(A0))
     * at 12 K helpers / 100 M cyc.
     *
     * jit_arg1 = src_addr (pre-computed by JIT: An + sext16(d16))
     * jit_arg2 = dst_an (0..7) — helper reads cpu->a[dst_an] for dst */
    u32 src_addr = cpu->jit_arg1;
    int dst_an = (int)(cpu->jit_arg2 & 7);
    u32 dst_addr = cpu->a[dst_an];
    u8 v = mac_read8(cpu->mem, src_addr);
    mac_write8(cpu->mem, dst_addr, v);
    u8 ccr = m68k_get_ccr(cpu) & CCR_X;
    if (v == 0)        ccr |= CCR_Z;
    if (v & 0x80)      ccr |= CCR_N;
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

/* ============================================================
 * 68020/68030 ISA additions (M7.0 — SE/30 foundation milestone)
 * ============================================================
 *
 * These helpers implement the integer-ISA extensions that the 68030 in the
 * Mac SE/30 needs. They are gated by encoding only — the bit patterns
 * never collide with valid 68000 encodings, so adding them is safe for
 * Plus mode (Plus ROM never emits them; existing handlers either don't
 * match or raise illegal-instruction, both of which the new handlers
 * supersede only when the new patterns actually fire).
 *
 * Coverage:
 *   bitfield      BFTST / BFEXTU / BFEXTS / BFCHG / BFCLR / BFSET / BFFFO / BFINS
 *   long mul/div  MULU.L / MULS.L (32×32, 32×32→64) / DIVU.L / DIVS.L (32÷32, 64÷32)
 *   short adds    EXTB.L / LINK.L / RTD #d16
 *   control regs  MOVEC ctl-reg<->Rn (SFC/DFC/USP/VBR/CACR/CAAR/MSP/ISP)
 *   moves         MOVES <ea>,Rn / Rn,<ea>  (function code modes — modelled as plain access)
 *   trap          TRAPcc (with optional .W / .L operand)
 *   bounds        CHK2 / CMP2
 *   compare-swap  CAS / CAS2 (non-atomic — the guest is single-core)
 *   bcd packing   PACK / UNPK
 *   PMMU          PMOVE TC/SRP/CRP/TT0/TT1/MMUSR + PTEST → MMUSR demux
 *                 PFLUSH / PLOAD — accept-and-advance (no TLB)
 *                 Full multi-level walk: short + long form, TT0/TT1,
 *                 WP/S/IS/U/M enforcement (M7.6g-p)
 *   cache         CINV / CPUSH — no-op advance */

static bool is_priv_violation_if_user(m68k_cpu *cpu, u32 op_pc) {
    if (!m68k_is_super(cpu)) {
        m68k_unimpl(cpu, op_pc, 8);   /* privilege violation */
        return true;
    }
    return false;
}

/* ---- bitfield ops ----------------------------------------------------- */

static u32 bf_offset_resolve(m68k_cpu *cpu, u16 ext) {
    /* Bit 11 = Do (offset dynamic via Dn). Bits 10..6 = offset value or Dn idx. */
    if (ext & 0x0800) return cpu->d[(ext >> 6) & 7];
    return (ext >> 6) & 0x1F;
}

static u32 bf_width_resolve(m68k_cpu *cpu, u16 ext) {
    /* Bit 5 = Dw (width dynamic via Dn). Bits 4..0 = width (0 means 32). */
    u32 w;
    if (ext & 0x0020) w = cpu->d[ext & 7] & 0x1F;
    else              w = ext & 0x1F;
    if (w == 0) w = 32;
    return w;
}

/* Read up to 5 bytes covering a bitfield window starting at byte_addr,
 * with bit_off in [0..7], width in [1..32]. Returns the field right-
 * aligned in a u32 (bits above the field are zero). */
static u32 bf_read_mem(m68k_cpu *cpu, u32 byte_addr, u32 bit_off, u32 width) {
    u32 total_bits = bit_off + width;
    u32 nbytes = (total_bits + 7u) / 8u;   /* 1..5 */
    u64 v = 0;
    for (u32 i = 0; i < nbytes; i++)
        v = (v << 8) | mac_read8(cpu->mem, byte_addr + i);
    u32 shift = (nbytes * 8u) - (bit_off + width);
    return (u32)((v >> shift) & ((1ull << width) - 1ull));
}

static void bf_write_mem(m68k_cpu *cpu, u32 byte_addr, u32 bit_off,
                         u32 width, u32 field) {
    u32 total_bits = bit_off + width;
    u32 nbytes = (total_bits + 7u) / 8u;
    u64 v = 0;
    for (u32 i = 0; i < nbytes; i++)
        v = (v << 8) | mac_read8(cpu->mem, byte_addr + i);
    u32 shift = (nbytes * 8u) - (bit_off + width);
    u64 mask = ((1ull << width) - 1ull) << shift;
    v = (v & ~mask) | (((u64)field & ((1ull << width) - 1ull)) << shift);
    for (u32 i = 0; i < nbytes; i++)
        mac_write8(cpu->mem, byte_addr + (nbytes - 1 - i), (u8)((v >> (i * 8)) & 0xFF));
}

static void do_bitfield(m68k_cpu *cpu, u16 op, u32 op_pc) {
    u16 ext = fetch16(cpu);
    int op_sel = (op >> 8) & 7;   /* 0=BFTST,1=BFEXTU,2=BFCHG,3=BFEXTS,
                                   * 4=BFCLR,5=BFFFO,6=BFSET,7=BFINS */
    int mode = (op >> 3) & 7, reg = op & 7;
    int dn_arg = (ext >> 12) & 7;
    u32 offset = bf_offset_resolve(cpu, ext);
    u32 width  = bf_width_resolve(cpu, ext);
    bool is_reg = (mode == 0);
    if (is_reg && width > 32) width = 32;

    /* Read the field. */
    u32 field;
    u32 byte_addr = 0, bit_off = 0;
    u32 rdn = 0;
    if (is_reg) {
        /* Register operand: offset is taken mod 32; width clipped to 32. */
        u32 off32 = offset & 31u;
        rdn = cpu->d[reg];
        /* Bits in field are bits (31-off..31-off-width+1) of Dn. */
        u32 left_shift = off32;
        u32 right_shift = 32u - width;
        /* Sign-aware: BFEXTS does an arithmetic right shift below; here we
         * just produce the bits zero-extended. */
        field = (rdn << left_shift) >> right_shift;
    } else {
        ea_t e = ea_decode(cpu, mode, reg, 1);
        if (e.kind != EA_MEM) { illegal(cpu, op); return; }
        i32 soff = (i32)offset;
        byte_addr = (u32)((i32)e.addr + (soff >> 3));
        bit_off = (u32)(soff & 7);
        if (soff < 0) {
            /* For negative offsets (only allowed when the offset is dynamic),
             * normalise so bit_off is in [0..7]. */
            if (bit_off != 0) {
                bit_off = (u32)(8 - (((-soff) - 1) % 8 + 1));
            }
        }
        field = bf_read_mem(cpu, byte_addr, bit_off, width);
    }

    /* CCR from the field (Z = field == 0; N = MSB of field). V = C = 0. */
    u8 ccr = m68k_get_ccr(cpu) & CCR_X;
    if (field == 0) ccr |= CCR_Z;
    if (field & (1u << (width - 1))) ccr |= CCR_N;
    m68k_set_ccr(cpu, ccr);

    switch (op_sel) {
    case 0:   /* BFTST: flags only */
        break;
    case 1: { /* BFEXTU */
        cpu->d[dn_arg] = field;
        break;
    }
    case 3: { /* BFEXTS */
        u32 mask = (width >= 32) ? 0xFFFFFFFFu : ((1u << width) - 1u);
        if (field & (1u << (width - 1)))
            cpu->d[dn_arg] = field | ~mask;
        else
            cpu->d[dn_arg] = field;
        break;
    }
    case 5: { /* BFFFO: find first one. Result = offset + leading-zero count
               * within the width-wide window. If all zero, result = offset + width. */
        u32 lz = 0;
        for (u32 i = 0; i < width; i++) {
            if (field & (1u << (width - 1 - i))) break;
            lz++;
        }
        cpu->d[dn_arg] = offset + lz;
        break;
    }
    case 2:   /* BFCHG */
    case 4:   /* BFCLR */
    case 6: { /* BFSET */
        u32 new_field = (op_sel == 2) ? (~field) : (op_sel == 4) ? 0u : 0xFFFFFFFFu;
        u32 mask = (width >= 32) ? 0xFFFFFFFFu : ((1u << width) - 1u);
        new_field &= mask;
        if (is_reg) {
            /* Write back into Dn at the same bit range. */
            u32 off32 = offset & 31u;
            u32 left = (32u - width - off32) & 31u;
            u32 dst_mask = mask << left;
            cpu->d[reg] = (cpu->d[reg] & ~dst_mask) | ((new_field << left) & dst_mask);
        } else {
            bf_write_mem(cpu, byte_addr, bit_off, width, new_field);
        }
        break;
    }
    case 7: { /* BFINS */
        u32 src = cpu->d[dn_arg];
        u32 mask = (width >= 32) ? 0xFFFFFFFFu : ((1u << width) - 1u);
        u32 new_field = src & mask;
        /* CCR for BFINS is computed from the source-field bits, not the
         * pre-existing memory. Recompute. */
        ccr = m68k_get_ccr(cpu) & CCR_X;
        if (new_field == 0) ccr |= CCR_Z;
        if (new_field & (1u << (width - 1))) ccr |= CCR_N;
        m68k_set_ccr(cpu, ccr);
        if (is_reg) {
            u32 off32 = offset & 31u;
            u32 left = (32u - width - off32) & 31u;
            u32 dst_mask = mask << left;
            cpu->d[reg] = (cpu->d[reg] & ~dst_mask) | ((new_field << left) & dst_mask);
        } else {
            bf_write_mem(cpu, byte_addr, bit_off, width, new_field);
        }
        break;
    }
    default: illegal(cpu, op); return;
    }
    cpu->cycles += is_reg ? 6 : 12;
    (void)op_pc;
}

/* ---- long multiply / divide ------------------------------------------- */

static void do_long_muldiv(m68k_cpu *cpu, u16 op, u32 op_pc) {
    bool is_div = (op & 0x0040) != 0;
    int mode = (op >> 3) & 7, reg = op & 7;
    u16 ext = fetch16(cpu);
    bool sgn = (ext & 0x0800) != 0;
    int dq = (ext >> 12) & 7;
    int dr = ext & 7;
    bool sz64 = (ext & 0x0400) != 0;
    ea_t e = ea_decode(cpu, mode, reg, 4);
    u32 src = ea_read(cpu, &e, 4);
    if (!is_div) {
        /* MULU.L / MULS.L */
        if (sz64) {
            /* 32 × 32 -> 64-bit; high half in Dr, low half in Dq. */
            u64 a = sgn ? (u64)(i64)(i32)cpu->d[dq] : (u64)cpu->d[dq];
            u64 b = sgn ? (u64)(i64)(i32)src        : (u64)src;
            u64 r = sgn ? (u64)((i64)a * (i64)b) : a * b;
            cpu->d[dq] = (u32)(r & 0xFFFFFFFFu);
            cpu->d[dr] = (u32)(r >> 32);
            u8 ccr = m68k_get_ccr(cpu) & CCR_X;
            if (r == 0) ccr |= CCR_Z;
            if ((i64)r < 0 && sgn) ccr |= CCR_N;
            else if (!sgn && (r >> 63)) ccr |= CCR_N;
            m68k_set_ccr(cpu, ccr);
        } else {
            /* 32 × 32 -> 32-bit result in Dq; V set if result overflows 32 bits. */
            u64 a = sgn ? (u64)(i64)(i32)cpu->d[dq] : (u64)cpu->d[dq];
            u64 b = sgn ? (u64)(i64)(i32)src        : (u64)src;
            u64 r = sgn ? (u64)((i64)a * (i64)b) : a * b;
            u32 r32 = (u32)(r & 0xFFFFFFFFu);
            cpu->d[dq] = r32;
            u8 ccr = m68k_get_ccr(cpu) & CCR_X;
            if (r32 == 0) ccr |= CCR_Z;
            if (r32 & 0x80000000u) ccr |= CCR_N;
            bool overflow;
            if (sgn) {
                i64 sr = (i64)r;
                overflow = sr < (i64)(i32)0x80000000 || sr > (i64)(i32)0x7FFFFFFF;
            } else {
                overflow = (r >> 32) != 0;
            }
            if (overflow) ccr |= CCR_V;
            m68k_set_ccr(cpu, ccr);
        }
        cpu->cycles += 40;
    } else {
        /* DIVU.L / DIVS.L */
        if (src == 0) { m68k_exception(cpu, 5); return; }
        if (sz64) {
            /* 64 ÷ 32 -> 32q + 32r. Dividend = (Dr:Dq); quotient in Dq, remainder in Dr. */
            if (sgn) {
                i64 dividend = (i64)(((u64)cpu->d[dr] << 32) | cpu->d[dq]);
                i32 divisor = (i32)src;
                i64 q = dividend / divisor;
                i64 r = dividend % divisor;
                if (q > (i64)(i32)0x7FFFFFFF || q < (i64)(i32)0x80000000) {
                    u8 ccr = m68k_get_ccr(cpu) & CCR_X;
                    ccr |= CCR_V;
                    m68k_set_ccr(cpu, ccr);
                } else {
                    cpu->d[dq] = (u32)(i32)q;
                    cpu->d[dr] = (u32)(i32)r;
                    u8 ccr = m68k_get_ccr(cpu) & CCR_X;
                    if (q == 0) ccr |= CCR_Z;
                    if (q < 0)  ccr |= CCR_N;
                    m68k_set_ccr(cpu, ccr);
                }
            } else {
                u64 dividend = ((u64)cpu->d[dr] << 32) | cpu->d[dq];
                u32 divisor = src;
                u64 q = dividend / divisor;
                u64 r = dividend % divisor;
                if (q > 0xFFFFFFFFu) {
                    u8 ccr = m68k_get_ccr(cpu) & CCR_X;
                    ccr |= CCR_V;
                    m68k_set_ccr(cpu, ccr);
                } else {
                    cpu->d[dq] = (u32)q;
                    cpu->d[dr] = (u32)r;
                    u8 ccr = m68k_get_ccr(cpu) & CCR_X;
                    if (q == 0) ccr |= CCR_Z;
                    if (q & 0x80000000u) ccr |= CCR_N;
                    m68k_set_ccr(cpu, ccr);
                }
            }
        } else {
            /* 32 ÷ 32 -> 32q in Dq, 32r in Dr. */
            if (sgn) {
                i32 dividend = (i32)cpu->d[dq];
                i32 divisor  = (i32)src;
                if (divisor == 0) { m68k_exception(cpu, 5); return; }
                /* Avoid INT_MIN / -1 overflow trap (undefined in C). */
                i32 q, r;
                if (dividend == (i32)0x80000000 && divisor == -1) {
                    u8 ccr = m68k_get_ccr(cpu) & CCR_X;
                    ccr |= CCR_V;
                    m68k_set_ccr(cpu, ccr);
                    cpu->cycles += 80;
                    return;
                }
                q = dividend / divisor;
                r = dividend - q * divisor;
                cpu->d[dq] = (u32)q;
                if (dr != dq) cpu->d[dr] = (u32)r;
                u8 ccr = m68k_get_ccr(cpu) & CCR_X;
                if (q == 0) ccr |= CCR_Z;
                if (q < 0)  ccr |= CCR_N;
                m68k_set_ccr(cpu, ccr);
            } else {
                u32 q = cpu->d[dq] / src;
                u32 r = cpu->d[dq] - q * src;
                cpu->d[dq] = q;
                if (dr != dq) cpu->d[dr] = r;
                u8 ccr = m68k_get_ccr(cpu) & CCR_X;
                if (q == 0) ccr |= CCR_Z;
                if (q & 0x80000000u) ccr |= CCR_N;
                m68k_set_ccr(cpu, ccr);
            }
        }
        cpu->cycles += 80;
    }
    (void)op_pc;
}

/* ---- MOVEC ------------------------------------------------------------ */

static u32 *movec_ctlreg(m68k_cpu *cpu, u16 sel) {
    /* Encodings from the 68030 PRM. We accept the 68020 set plus the 68030
     * PMMU registers (TC, ITT0/1, DTT0/1, SRP, CRP, MMUSR) — see the
     * separate PMMU helpers for the wider TC/SRP/CRP. Only 32-bit regs
     * are returned through this pointer; SRP/CRP are 64-bit and handled
     * directly. */
    switch (sel) {
    case 0x000: return &cpu->sfc;
    case 0x001: return &cpu->dfc;
    case 0x002: return &cpu->cacr;
    case 0x800: return &cpu->usp;
    case 0x801: return &cpu->vbr;
    case 0x802: return &cpu->caar;
    case 0x803: return &cpu->msp;
    case 0x804: return &cpu->isp;
    case 0x003: return &cpu->tc;     /* PMMU TC — 68030 */
    case 0x004: return &cpu->tt0;
    case 0x005: return &cpu->tt1;
    default:    return NULL;
    }
}

static void do_movec(m68k_cpu *cpu, u16 op, u32 op_pc) {
    if (is_priv_violation_if_user(cpu, op_pc)) return;
    u16 ext = fetch16(cpu);
    /* Direction is encoded in OPCODE bit 0:
     *   0x4E7A: bit 0 = 0 → ctl-reg → Rn
     *   0x4E7B: bit 0 = 1 → Rn → ctl-reg
     * Ext word bit 15 = A/D flag for the general register. */
    int dir_to_ctl = (op & 1) != 0;
    int gen_reg = (ext >> 12) & 0xF; /* bit 15 = A/D, bits 14-12 = idx */
    u32 *p = movec_ctlreg(cpu, ext & 0xFFF);
    if (!p) { illegal(cpu, op); return; }
    u32 *gr = (gen_reg < 8) ? &cpu->d[gen_reg] : &cpu->a[gen_reg - 8];
    if (dir_to_ctl) *p = *gr;
    else            *gr = *p;
    cpu->cycles += 12;
}

/* ---- MOVES ----------------------------------------------------------- */
static void do_moves(m68k_cpu *cpu, u16 op, u32 op_pc) {
    if (is_priv_violation_if_user(cpu, op_pc)) return;
    u16 ext = fetch16(cpu);
    int sz = ((op >> 6) & 3);
    sz = sz == 0 ? 1 : sz == 1 ? 2 : 4;
    int mode = (op >> 3) & 7, reg = op & 7;
    int gen_reg = (ext >> 12) & 0xF;
    bool to_mem = (ext & 0x0800) != 0;
    u32 *gr = (gen_reg < 8) ? &cpu->d[gen_reg] : &cpu->a[gen_reg - 8];
    ea_t e = ea_decode(cpu, mode, reg, sz);
    if (to_mem) {
        u32 v = (gen_reg < 8) ? (*gr & size_mask(sz)) : *gr;
        ea_write(cpu, &e, sz, v);
    } else {
        u32 v = ea_read(cpu, &e, sz);
        if (gen_reg < 8) {
            cpu->d[gen_reg] = (cpu->d[gen_reg] & ~size_mask(sz)) | (v & size_mask(sz));
        } else {
            cpu->a[gen_reg - 8] = sext(v, sz);
        }
    }
    cpu->cycles += 12;
}

/* ---- TRAPcc / TRAPV ---------------------------------------------------- */
static void do_trapcc(m68k_cpu *cpu, u16 op, u32 op_pc) {
    int cc = (op >> 8) & 0xF;
    int opmode = op & 7;
    if (opmode == 2) fetch16(cpu);              /* skip .W operand */
    else if (opmode == 3) fetch32(cpu);         /* skip .L operand */
    /* opmode == 4: no operand. Anything else is illegal. */
    if (opmode != 2 && opmode != 3 && opmode != 4) {
        illegal(cpu, op); return;
    }
    if (cond_true(cpu, cc)) m68k_exception(cpu, 7);
    (void)op_pc;
}

/* ---- CHK2 / CMP2 ----------------------------------------------------- */
static void do_chk2_cmp2(m68k_cpu *cpu, u16 op, u32 op_pc) {
    u16 ext = fetch16(cpu);
    int sz = (op >> 9) & 3;
    sz = sz == 0 ? 1 : sz == 1 ? 2 : 4;
    int mode = (op >> 3) & 7, reg = op & 7;
    bool is_chk2 = (ext & 0x0800) != 0;
    int regn = (ext >> 12) & 0xF;
    bool is_addr = (regn & 8) != 0;
    ea_t e = ea_decode(cpu, mode, reg, sz);
    u32 lo = ea_read(cpu, &e, sz);
    u32 hi;
    if (sz == 1)      hi = mac_read8 (cpu->mem, e.addr + 1);
    else if (sz == 2) hi = mac_read16(cpu->mem, e.addr + 2);
    else              hi = mac_read32(cpu->mem, e.addr + 4);
    i32 ilo = (i32)sext(lo, sz), ihi = (i32)sext(hi, sz);
    i32 v;
    if (is_addr) v = (i32)cpu->a[regn & 7];
    else         v = (i32)sext(cpu->d[regn & 7], sz);
    u8 ccr = m68k_get_ccr(cpu) & CCR_X;
    bool out_of_range = (v < ilo) || (v > ihi);
    bool equal_bound  = (v == ilo) || (v == ihi);
    if (equal_bound) ccr |= CCR_Z;
    if (out_of_range) ccr |= CCR_C;
    m68k_set_ccr(cpu, ccr);
    if (is_chk2 && out_of_range) m68k_exception(cpu, 6);
    cpu->cycles += 18;
    (void)op_pc;
}

/* ---- CAS / CAS2 ------------------------------------------------------ */
static void do_cas(m68k_cpu *cpu, u16 op, u32 op_pc) {
    u16 ext = fetch16(cpu);
    int sz = (op >> 9) & 3;
    sz = sz == 1 ? 1 : sz == 2 ? 2 : 4;
    int mode = (op >> 3) & 7, reg = op & 7;
    int dc = ext & 7;
    int du = (ext >> 6) & 7;
    ea_t e = ea_decode(cpu, mode, reg, sz);
    u32 mem = ea_read(cpu, &e, sz);
    u32 cmp = cpu->d[dc] & size_mask(sz);
    u32 r = mem - cmp;
    set_flags_cmp(cpu, sz, cmp, mem, r);
    if ((mem & size_mask(sz)) == cmp)
        ea_write(cpu, &e, sz, cpu->d[du]);
    else {
        cpu->d[dc] = (cpu->d[dc] & ~size_mask(sz)) | (mem & size_mask(sz));
    }
    cpu->cycles += 18;
    (void)op_pc;
}

static void do_cas2(m68k_cpu *cpu, u16 op, u32 op_pc) {
    u16 ext1 = fetch16(cpu);
    u16 ext2 = fetch16(cpu);
    int sz = ((op & 0x0200) != 0) ? 4 : 2;
    int rn1 = (ext1 >> 12) & 0xF, rn2 = (ext2 >> 12) & 0xF;
    int du1 = (ext1 >> 6) & 7,    du2 = (ext2 >> 6) & 7;
    int dc1 = ext1 & 7,           dc2 = ext2 & 7;
    u32 addr1 = (rn1 & 8) ? cpu->a[rn1 & 7] : cpu->d[rn1 & 7];
    u32 addr2 = (rn2 & 8) ? cpu->a[rn2 & 7] : cpu->d[rn2 & 7];
    u32 m1 = (sz == 2) ? mac_read16(cpu->mem, addr1) : mac_read32(cpu->mem, addr1);
    u32 m2 = (sz == 2) ? mac_read16(cpu->mem, addr2) : mac_read32(cpu->mem, addr2);
    u32 c1 = cpu->d[dc1] & size_mask(sz);
    u32 c2 = cpu->d[dc2] & size_mask(sz);
    u32 r1 = m1 - c1;
    set_flags_cmp(cpu, sz, c1, m1, r1);
    if ((m1 & size_mask(sz)) == c1) {
        u32 r2 = m2 - c2;
        set_flags_cmp(cpu, sz, c2, m2, r2);
        if ((m2 & size_mask(sz)) == c2) {
            if (sz == 2) {
                mac_write16(cpu->mem, addr1, (u16)cpu->d[du1]);
                mac_write16(cpu->mem, addr2, (u16)cpu->d[du2]);
            } else {
                mac_write32(cpu->mem, addr1, cpu->d[du1]);
                mac_write32(cpu->mem, addr2, cpu->d[du2]);
            }
            cpu->cycles += 24;
            (void)op_pc;
            return;
        }
        cpu->d[dc1] = (cpu->d[dc1] & ~size_mask(sz)) | (m1 & size_mask(sz));
        cpu->d[dc2] = (cpu->d[dc2] & ~size_mask(sz)) | (m2 & size_mask(sz));
    } else {
        cpu->d[dc1] = (cpu->d[dc1] & ~size_mask(sz)) | (m1 & size_mask(sz));
    }
    cpu->cycles += 24;
    (void)op_pc;
}

/* ---- PACK / UNPK ----------------------------------------------------- */
static void do_pack(m68k_cpu *cpu, u16 op, u32 op_pc) {
    int dy = op & 7, dx = (op >> 9) & 7;
    bool predec = (op & 0x0008) != 0;
    u16 adj = fetch16(cpu);
    u16 src;
    if (predec) {
        cpu->a[dy] -= 2;
        u8 hi = mac_read8(cpu->mem, cpu->a[dy]);
        u8 lo = mac_read8(cpu->mem, cpu->a[dy] + 1);
        src = (u16)(((u16)hi << 8) | lo);
    } else {
        src = (u16)(cpu->d[dy] & 0xFFFF);
    }
    u16 sum = (u16)(src + adj);
    u8 packed = (u8)(((sum >> 4) & 0xF0) | (sum & 0x0F));
    if (predec) {
        cpu->a[dx] -= 1;
        mac_write8(cpu->mem, cpu->a[dx], packed);
    } else {
        cpu->d[dx] = (cpu->d[dx] & 0xFFFFFF00u) | packed;
    }
    cpu->cycles += 6;
    (void)op_pc;
}

static void do_unpk(m68k_cpu *cpu, u16 op, u32 op_pc) {
    int dy = op & 7, dx = (op >> 9) & 7;
    bool predec = (op & 0x0008) != 0;
    u16 adj = fetch16(cpu);
    u8 src;
    if (predec) {
        cpu->a[dy] -= 1;
        src = mac_read8(cpu->mem, cpu->a[dy]);
    } else {
        src = (u8)(cpu->d[dy] & 0xFF);
    }
    u16 unp = (u16)(((src & 0xF0) << 4) | (src & 0x0F));
    unp = (u16)(unp + adj);
    if (predec) {
        cpu->a[dx] -= 2;
        mac_write8(cpu->mem, cpu->a[dx],     (u8)(unp >> 8));
        mac_write8(cpu->mem, cpu->a[dx] + 1, (u8)(unp & 0xFF));
    } else {
        cpu->d[dx] = (cpu->d[dx] & 0xFFFF0000u) | unp;
    }
    cpu->cycles += 8;
    (void)op_pc;
}

/* ---- PMMU + coprocessor (line F) ------------------------------------- */

/* PMMU instructions. PMOVE writes/reads the cpu->tc/srp/crp/tt0/tt1/mmusr
 * registers. PTEST walks the page table and demuxes BERR cause into
 * MMUSR W/S/I bits. PFLUSH/PLOAD are accept-and-advance (no TLB to
 * flush in this model). Full translation lives in mac_pmmu_translate
 * — see core/mac_mem.c (M7.6g-p milestones). */
static void do_pmmu(m68k_cpu *cpu, u16 op, u32 op_pc) {
    if (is_priv_violation_if_user(cpu, op_pc)) return;
    u16 ext = fetch16(cpu);
    int mode = (op >> 3) & 7, reg = op & 7;
    int p_op = (ext >> 13) & 7;
    /* PMOVE: ext bits 15-13 = 010 (to/from MMU register), 011 (with FD bit). */
    if (p_op == 2 || p_op == 3) {
        bool to_mmu = (ext & 0x0200) == 0;        /* 0 = mem->MMU, 1 = MMU->mem */
        int p_reg = (ext >> 10) & 7;              /* 0=TC, 2=SRP, 3=CRP */
        ea_t e = ea_decode(cpu, mode, reg, 4);
        if (e.kind != EA_MEM) { illegal(cpu, op); return; }
        if (to_mmu) {
            if (p_reg == 0) {
                cpu->tc = mac_read32(cpu->mem, e.addr);
            } else if (p_reg == 2 || p_reg == 3) {
                u64 hi = mac_read32(cpu->mem, e.addr);
                u64 lo = mac_read32(cpu->mem, e.addr + 4);
                if (p_reg == 2) cpu->srp = (hi << 32) | lo;
                else            cpu->crp = (hi << 32) | lo;
            }
            /* Other p_reg values (TT0=4, TT1=5, MMUSR=6) — write through to
             * tt0/tt1/mmusr. Not all combos are valid on 68030 but the ROM's
             * write sequence is what counts. */
            if (p_reg == 4)      cpu->tt0 = mac_read32(cpu->mem, e.addr);
            else if (p_reg == 5) cpu->tt1 = mac_read32(cpu->mem, e.addr);
            else if (p_reg == 6) cpu->mmusr = (u16)mac_read16(cpu->mem, e.addr);
        } else {
            if (p_reg == 0)       mac_write32(cpu->mem, e.addr, cpu->tc);
            else if (p_reg == 2)  { mac_write32(cpu->mem, e.addr,     (u32)(cpu->srp >> 32));
                                    mac_write32(cpu->mem, e.addr + 4, (u32)(cpu->srp & 0xFFFFFFFFu)); }
            else if (p_reg == 3)  { mac_write32(cpu->mem, e.addr,     (u32)(cpu->crp >> 32));
                                    mac_write32(cpu->mem, e.addr + 4, (u32)(cpu->crp & 0xFFFFFFFFu)); }
            else if (p_reg == 4)  mac_write32(cpu->mem, e.addr, cpu->tt0);
            else if (p_reg == 5)  mac_write32(cpu->mem, e.addr, cpu->tt1);
            else if (p_reg == 6)  mac_write16(cpu->mem, e.addr, cpu->mmusr);
        }
        cpu->cycles += 8;
        return;
    }
    /* PTEST (ext bits 15-13 = 100): walk the page table for the LA in
     * <ea> and populate MMUSR. Ext bit 9 = R/W (1 = read, 0 = write).
     * Ext bits 12-10 = function code (we extract supervisor from FC&4).
     * The walk should NOT actually raise a bus error — save/restore
     * cpu->bus_error_pending around the call and reflect status in
     * MMUSR bit 11 (I = invalid) on BERR, bit 12 (W = write-protected
     * — set on the WP path), bit 13 (S = supervisor violation). */
    if ((ext & 0xE000) == 0x8000) {
        ea_t e = ea_decode(cpu, mode, reg, 4);
        u32 la = (e.kind == EA_MEM) ? e.addr : 0;
        bool rw = (ext & (1u << 9)) != 0;     /* 1 = read */
        u8 fc = (u8)((ext >> 10) & 7);
        u32 saved_berr = cpu->bus_error_pending;
        cpu->bus_error_pending = 0;
        u32 phys = mac_pmmu_translate(cpu->mem, la, fc, !rw);
        u16 msr = 0;
        if (cpu->bus_error_pending != 0) {
            u32 cause = cpu->bus_error_pending & BERR_CAUSE_MASK;
            switch (cause) {
                case BERR_CAUSE_WP:      msr |= (1u << 12); break;   /* W */
                case BERR_CAUSE_SUPER:   msr |= (1u << 13); break;   /* S */
                case BERR_CAUSE_INVALID: msr |= (1u << 11); break;   /* I */
                case BERR_CAUSE_OOR:     msr |= (1u << 11); break;   /* I (limit-equiv) */
                /* CAUSE_GENERIC originates from non-PMMU code (unmapped MMIO
                 * read/write at the leaf accessors). Inside a walk it means
                 * the descriptor read itself faulted — MMUSR bit 15 = B. */
                case BERR_CAUSE_GENERIC:
                default:                 msr |= (1u << 15); break;   /* B */
            }
        }
        (void)phys;
        cpu->mmusr = msr;
        cpu->bus_error_pending = saved_berr;   /* PTEST must not raise BERR */
        cpu->cycles += 8;
        return;
    }
    /* PFLUSH / PLOAD — accept and advance. */
    if ((ext & 0xE000) == 0x2000 || (ext & 0xE000) == 0x0000) {
        if (mode > 1) ea_decode(cpu, mode, reg, 4);
        cpu->cycles += 8;
        return;
    }
    illegal(cpu, op);
}

/* CINV / CPUSH — no-op advance. The ext word is implicit in the opcode. */
static void do_cache(m68k_cpu *cpu, u16 op, u32 op_pc) {
    if (is_priv_violation_if_user(cpu, op_pc)) return;
    (void)op;
    cpu->cycles += 8;
}

/* ---- predicate the JIT block walker consults ------------------------- */
bool is_68020_only(u16 op) {
    /* Bitfield: BFTST/BFEXTU/BFCHG/BFEXTS/BFCLR/BFFFO/BFSET/BFINS — all
     * 8 forms share bits 15-11 = 11101 and bits 7-6 = 11, distinguished
     * only by bits 10-8 which the mask 0xF8C0 erases. */
    if ((op & 0xF8C0) == 0xE8C0) return true;
    /* MULU.L / MULS.L / DIVU.L / DIVS.L */
    if ((op & 0xFFC0) == 0x4C00 || (op & 0xFFC0) == 0x4C40) return true;
    /* EXTB.L */
    if ((op & 0xFFF8) == 0x49C0) return true;
    /* LINK.L */
    if ((op & 0xFFF8) == 0x4808) return true;
    /* RTD */
    if (op == 0x4E74) return true;
    /* MOVEC */
    if (op == 0x4E7A || op == 0x4E7B) return true;
    /* MOVES */
    if ((op & 0xFFC0) == 0x0E00 || (op & 0xFFC0) == 0x0E40
        || (op & 0xFFC0) == 0x0E80) return true;
    /* TRAPcc */
    if ((op & 0xF0F8) == 0x50F8 && (op & 0x0007) >= 2) return true;
    /* CHK2 / CMP2: same encoding shape as the reserved-sz ORI/ANDI/etc
     * forms; we only treat sz != 3 as the 020+ CHK2/CMP2. */
    if ((op & 0xF9C0) == 0x00C0 && ((op >> 9) & 3) != 3) return true;
    /* CAS — bits 10-9 (size field) must be nonzero; sz=00 is BSET static
     * on 68000, not CAS. */
    if ((op & 0xF9C0) == 0x08C0 && ((op >> 9) & 3) != 0) return true;
    /* CAS2 — fixed encodings only. */
    if (op == 0x0CFC || op == 0x0EFC) return true;
    /* PACK (line 8) / UNPK (line C). Both have a 16-bit adjustment word
     * following the opcode. Earlier this file's UNPK mask was 0x8180
     * which is on line 8 — but UNPK lives on line C (0xC180+). */
    if ((op & 0xF1F0) == 0x8140) return true;   /* PACK */
    if ((op & 0xF1F0) == 0xC180) return true;   /* UNPK */
    /* PMMU coprocessor (line F, cp-id = 0) */
    if ((op & 0xFE00) == 0xF000) return true;
    /* CINV / CPUSH */
    if ((op & 0xFF20) == 0xF400 || (op & 0xFF20) == 0xF420) return true;
    return false;
}

bool m68k_jit_can_inline_020(u16 op) {
    /* M7.5a — EXTB.L Dn: 0100 1001 1100 0nnn (0x49C0-0x49C7).
     * Native 2-op codegen in jit/codegen.c. */
    if ((op & 0xFFF8) == 0x49C0) return true;

    /* M7.5c — keep these in the block via the default m68k_step bridge
     * (no native inline arm yet — but the bridge correctly handles them
     * thanks to the M7.5b decoder-sizing fixes). The win is avoiding
     * a block-termination and the dispatcher round-trip on every 020+
     * op the SE/30 boot path hits. */
    if (op == 0x4E7A || op == 0x4E7B) return true;       /* MOVEC */
    if ((op & 0xFFF8) == 0x4808) return true;            /* LINK.L An,#d32 */
    if (op == 0x4E74) return true;                       /* RTD #d16 (block terminator) */
    if ((op & 0xFF20) == 0xF400) return true;            /* CINV  (line F cache) */
    if ((op & 0xFF20) == 0xF420) return true;            /* CPUSH (line F cache) */

    /* M7.5d — bitfield ops via m68k_step bridge. Decoder now correctly
     * sizes them (M7.5d in m68k_decode_at). All eight variants share
     * the mask 0xF8C0 == 0xE8C0. */
    if ((op & 0xF8C0) == 0xE8C0) return true;

    /* M7.5e — 32-bit MUL/DIV via m68k_step bridge. */
    if ((op & 0xFFC0) == 0x4C00) return true;   /* MULU.L / MULS.L */
    if ((op & 0xFFC0) == 0x4C40) return true;   /* DIVU.L / DIVS.L */

    /* M7.5f — line 0 020+ ops + TRAPcc + PACK/UNPK. All sized correctly
     * by m68k_decode_at and handled by m68k_step. */
    if ((op & 0xF9C0) == 0x00C0 && ((op >> 9) & 3) != 3) return true; /* CHK2/CMP2 */
    if (op == 0x0CFC || op == 0x0EFC) return true;                    /* CAS2 */
    if ((op & 0xF9C0) == 0x08C0 && ((op >> 9) & 3) != 0) return true; /* CAS */
    if ((op & 0xFFC0) == 0x0E00 || (op & 0xFFC0) == 0x0E40
        || (op & 0xFFC0) == 0x0E80) return true;                      /* MOVES */
    /* TRAPcc — opmode in {.W, .L, no-op} (reg 2/3/4 with mode=7, szf=3). */
    if ((op & 0xF0F8) == 0x50F8 && (op & 7) >= 2 && (op & 7) <= 4) return true;
    if ((op & 0xF1F0) == 0x8140) return true;   /* PACK */
    if ((op & 0xF1F0) == 0xC180) return true;   /* UNPK */

    /* M7.5g — PMMU coprocessor (line F, cp-id 0): PMOVE/PFLUSH/PLOAD/
     * PTEST. The interpreter does register-stub only (TODO(pmmu)) but
     * keeping in block avoids dispatcher round-trips.
     *
     * M7.6ad — DISABLED. The helper-step bridge for PMMU corrupts the
     * register file in a way that causes the next op in the block to
     * read garbage targets — observed via diff-jit-trace on SE/30
     * reset: block at 0x4083F872 (F010 PMOVE + 4EF9 JMP) sends JIT to
     * PC=0 instead of the JMP target. Terminating the block at PMMU
     * costs one dispatcher round-trip but keeps the JIT correct.
     *
     * M7.6am — re-enable attempted, immediately caught by the
     * diff_jit_se30_reset_lockstep test: even with M7.6ae's BERR
     * handling, the bug is independent and PMMU inline still
     * corrupts.
     *
     * M7.6an — ROOT CAUSE FOUND + FIXED: the JIT's F-line trap arm in
     * jit/codegen.c was catching PMMU ops and raising vec 11. Added a
     * guard BEFORE the F-line trap arm that lets PMMU fall through to
     * the helper_step bridge. Re-enabled here. Verified by the
     * lockstep test now passing with PMMU inline. */
    if ((op & 0xFE00) == 0xF000) return true;

    return false;
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
        /* 68020+ CHK2 / CMP2 / CAS / CAS2 sit on top of the same line-0
         * encoding space. CHK2/CMP2 use bits 7-6 = 11 with op9 in {0,1,2}
         * (size); CAS has op9 with bit 11 = 1 (so op9 in {4,5,6,7}) and
         * bits 7-6 = 11; CAS2 has the same prefix as CAS plus mode=7/
         * reg=4 (xxx).L addressing-style decode actually it's a fixed
         * encoding 0x0CFC / 0x0EFC. These must be filtered BEFORE the
         * Plus 68000 immediate / bit-op dispatch because they overlap
         * with mis-decoded ORI/EORI/CMPI for the .L size. */
        /* CHK2 / CMP2: bits 15-12=0, bit 11=0, bit 8=0, bits 7-6=11, sz
         * field (bits 10-9) in {00,01,10}. The reserved sz=11 case stays
         * in the existing immediate ALU dispatch — though 68000 never
         * emits it. */
        if ((op & 0xF9C0) == 0x00C0 && ((op >> 9) & 3) != 3) {
            do_chk2_cmp2(cpu, op, op_pc); return;
        }
        /* CAS2: fixed encodings 0x0CFC (.W) / 0x0EFC (.L). */
        if (op == 0x0CFC || op == 0x0EFC) { do_cas2(cpu, op, op_pc); return; }
        /* CAS: bits 15-12=0, bit 11=1, bit 8=0, bits 7-6=11, sz != 00.
         * The "sz != 00" check is essential: bits 11-8 = 1000 (i.e. sz=00)
         * is the 68000 static bit-op group (BTST/BCHG/BCLR/BSET #imm,<ea>),
         * not CAS. */
        if ((op & 0xF9C0) == 0x08C0 && ((op >> 9) & 3) != 0) {
            do_cas(cpu, op, op_pc); return;
        }
        /* MOVES — 0000 1110 ss mm mrrr (privileged). */
        if (((op & 0xFFC0) == 0x0E00) || ((op & 0xFFC0) == 0x0E40)
            || ((op & 0xFFC0) == 0x0E80)) {
            do_moves(cpu, op, op_pc); return;
        }
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
        /* 68020+ additions that fall in line-4: RTD, MOVEC, LINK.L, EXTB.L,
         * 32-bit MUL/DIV. Filter before the 68000 dispatch. */
        if (op == 0x4E74) {                                  /* RTD #d16 */
            i16 d = (i16)fetch16(cpu);
            cpu->pc = mac_read32(cpu->mem, cpu->a[7]);
            cpu->a[7] += 4 + (u32)(i32)d;
            cpu->cycles += 16;
            return;
        }
        if (op == 0x4E7A || op == 0x4E7B) {                  /* MOVEC */
            do_movec(cpu, op, op_pc); return;
        }
        if ((op & 0xFFF8) == 0x4808) {                       /* LINK.L An,#d32 */
            int an = op & 7;
            i32 d = (i32)fetch32(cpu);
            cpu->a[7] -= 4;
            mac_write32(cpu->mem, cpu->a[7], cpu->a[an]);
            cpu->a[an] = cpu->a[7];
            cpu->a[7] += (u32)d;
            cpu->cycles += 12;
            return;
        }
        if ((op & 0xFFF8) == 0x49C0) {                       /* EXTB.L Dn */
            int dn = op & 7;
            cpu->d[dn] = (u32)(i32)(i8)(u8)cpu->d[dn];
            set_flags_logic(cpu, 4, cpu->d[dn]);
            return;
        }
        if ((op & 0xFFC0) == 0x4C00 || (op & 0xFFC0) == 0x4C40) {
            /* MULU.L / MULS.L (0x4C00) / DIVU.L / DIVS.L (0x4C40). */
            do_long_muldiv(cpu, op, op_pc); return;
        }
        /* Fixed-encoding instructions first. */
        if (op == 0x4E71) { return; }                       /* NOP */
        if (op == 0x4E70) {
            /* RESET instruction: on real hardware this asserts the /RESET
             * pin for ~10 clocks (resetting all peripherals) and then
             * execution continues. We don't model the peripheral reset
             * yet — just advance and continue, which matches the SE/30
             * ROM's expectation (it RESETs early during POST and then
             * proceeds). Plus ROM never emits RESET so this is a no-op
             * change for the Plus lockstep tests.
             * TODO(reset): mac_peripherals_reset(cpu->mem). */
            cpu->cycles += 132;
            return;
        }
        if (op == 0x4E73) {                                  /* RTE */
            bool was = m68k_is_super(cpu);
            u16 sr = mac_read16(cpu->mem, cpu->a[7]);
            u32 pc = mac_read32(cpu->mem, cpu->a[7] + 2);
            /* M7.6r — 030 frame size depends on format word at SP+6.
             * Plus pops 6 bytes (no format word); 030 format-0 pops 8;
             * 030 format-A pops 32. */
            u32 frame_size = 6;
            if (cpu->mem && cpu->mem->model == MAC_MODEL_SE30) {
                u16 fmt_vec = mac_read16(cpu->mem, cpu->a[7] + 6);
                u32 fmt = (fmt_vec >> 12) & 0xFu;
                frame_size = (fmt == 0xA) ? 32u : 8u;
            }
            cpu->a[7] += frame_size;
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

    /* ---- 0101: ADDQ/SUBQ/Scc/DBcc/TRAPcc ----------------------------- */
    case 0x5: {
        int szf = (op >> 6) & 3;
        int mode = (op >> 3) & 7, reg = op & 7;
        /* 68020+ TRAPcc: 0101 cccc 11111 OPMODE, where OPMODE = 010/011/100
         * (.W / .L / no operand). Bits 5-3 = 11111 i.e. mode = 7 and reg
         * in {2,3,4}. */
        if (szf == 3 && mode == 7 && (reg == 2 || reg == 3 || reg == 4)) {
            do_trapcc(cpu, op, op_pc); return;
        }
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
        /* 68020+ PACK: 1000 ddd 1 0100 X rrr — distinguished from SBCD by
         * bits 8-3 being 1 0100X (PACK) vs 1 0000X (SBCD). */
        if ((op & 0x01F0) == 0x0140) { do_pack(cpu, op, op_pc); return; }
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
        /* 68020+ UNPK: 1100 ddd 1 1000 X rrr. */
        if ((op & 0x01F0) == 0x0180) { do_unpk(cpu, op, op_pc); return; }
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

    /* ---- 1110: shifts / rotates / bitfield (020+) -------------------- */
    case 0xE: {
        /* 68020+ bitfield: 1110 1XXX 11mm mrrr. Bits 10-8 select the op
         * (BFTST/BFEXTU/BFCHG/BFEXTS/BFCLR/BFFFO/BFSET/BFINS), bits 7-6 =
         * 11 distinguish from the shifts (bits 7-6 = szf, where szf=3
         * means memory shift not bitfield). The bitfield ops use bits
         * 7-6 = 11 + bit 11 = 1, while memory shifts have bit 11 = 0. */
        if ((op & 0xF8C0) == 0xE8C0) { do_bitfield(cpu, op, op_pc); return; }
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

    /* ---- 1111: line-F emulator + 68020+ coprocessor + cache --------- */
    case 0xF:
        /* CINV / CPUSH — 1111 0100 0CXX XXX (bits 11-8 = 0100, bit 5 = 0/1).
         * Both are no-ops in our model (no on-chip cache). */
        if ((op & 0xFF20) == 0xF400 || (op & 0xFF20) == 0xF420) {
            do_cache(cpu, op, op_pc); return;
        }
        /* PMMU instructions on the 68030 use line F coprocessor 0
         * (op bits 11-9 = 000). PMOVE/PFLUSH/PLOAD/PTEST decode here. */
        if ((op & 0xFE00) == 0xF000) {
            do_pmmu(cpu, op, op_pc); return;
        }
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
 * consumption. Returns extra bytes beyond the opcode word.
 *
 * Note: callers pass the instruction start as `pc` (not the actual ext
 * word position), so this function must remain a pure function of
 * (mode, reg, sz) — it cannot peek at the real ext word. The 68000 only
 * uses the brief format for modes 6 and 7.3 (2 bytes); the 68020+ "full"
 * format (4-10 bytes) is handled at JIT-walk time by is_68020_only
 * detecting bit 8 of the ext word and terminating the block, and at
 * interp-time by brief_index reading the full-format fields directly. */
static u32 ea_ext_bytes(m68k_cpu *cpu, u32 pc, int mode, int reg, int sz) {
    (void)cpu; (void)pc;
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
        default: return 0;
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
            /* M7.5f — 68020+ ops in line 0 (sized before the 68000 fall-
             * through). All have a mandatory ext word at op+2; CAS2 has
             * two ext words. */
            if ((op & 0xF9C0) == 0x00C0 && ((op >> 9) & 3) != 3) {
                /* CHK2 / CMP2 */
                d.length += 2 + ea_ext_bytes(cpu, pc + 2, mode, reg, 1);
                break;
            }
            if (op == 0x0CFC || op == 0x0EFC) {
                /* CAS2 — fixed 6 bytes (op + 2 ext words). */
                d.length += 4;
                break;
            }
            if ((op & 0xF9C0) == 0x08C0 && ((op >> 9) & 3) != 0) {
                /* CAS */
                d.length += 2 + ea_ext_bytes(cpu, pc + 2, mode, reg, 4);
                break;
            }
            if ((op & 0xFFC0) == 0x0E00 || (op & 0xFFC0) == 0x0E40
                || (op & 0xFFC0) == 0x0E80) {
                /* MOVES */
                d.length += 2 + ea_ext_bytes(cpu, pc + 2, mode, reg, 4);
                break;
            }
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
            /* M7.5b — RTD #d16 (68020+, 0x4E74): control-flow terminator,
             * 4 bytes (op + d16). Without ends_block the JIT block walker
             * keeps reading past the RTD and decodes wrong bytes; without
             * the +2 length the next op is mis-aligned. */
            if (op == 0x4E74) { d.length += 2; d.ends_block = true; break; }
            /* M7.5b — MOVEC Rc<->Rn (0x4E7A/0x4E7B): 4 bytes (op + ext).
             * Default fallthrough's mode-6 EA decode accidentally sizes
             * to 4, so this is just for clarity. Not a block terminator. */
            if (op == 0x4E7A || op == 0x4E7B) { d.length += 2; break; }
            if ((op & 0xFFC0) == 0x4EC0 || (op & 0xFFC0) == 0x4E80) {
                d.length += ea_ext_bytes(cpu, pc, mode, reg, 4);          /* JMP/JSR */
                d.ends_block = true; break;
            }
            if ((op & 0xFFF8) == 0x4E50) { d.length += 2; break; }        /* LINK */
            /* M7.5b — LINK.L An,#d32 (68020+, 0x4808-0x480F): 6 bytes
             * (op + d32). Without this the default fallthrough's mode-1
             * (An) sizes LINK.L to just 2 bytes and the walker decodes
             * the d32 displacement as 2 phantom instructions. */
            if ((op & 0xFFF8) == 0x4808) { d.length += 4; break; }
            /* M7.5e — MULU.L / MULS.L (0x4C00 + EA) and DIVU.L / DIVS.L
             * (0x4C40 + EA): 4 bytes minimum (op + 2-byte ext word) plus
             * EA bytes. The default fall-through misses the ext word. */
            if ((op & 0xFFC0) == 0x4C00 || (op & 0xFFC0) == 0x4C40) {
                d.length += 2 + ea_ext_bytes(cpu, pc + 2, mode, reg, 4);
                break;
            }
            /* 2-byte instructions with NO effective-address field — these
             * must not fall through to the generic EA-extension sizing,
             * which would mis-read their register bits as an EA mode. */
            if (op == 0x4E71 || op == 0x4E76 ||                           /* NOP, TRAPV */
                (op & 0xFFF8) == 0x4840 ||                                /* SWAP */
                (op & 0xFFB8) == 0x4880 ||                                /* EXT  */
                (op & 0xFFF8) == 0x49C0 ||                                /* EXTB.L (68020+) */
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
            /* M7.5f — TRAPcc (68020+): 0101 cccc 11111 OPMODE where
             * OPMODE in {010 (.W), 011 (.L), 100 (no operand)}. Encoded
             * as szf=3, mode=7, reg in {2,3,4}. .W form has 16-bit
             * operand; .L form has 32-bit operand; no-operand is just
             * 2 bytes. */
            if (szf == 3 && mode == 7 && (reg == 2 || reg == 3 || reg == 4)) {
                if (reg == 2)      d.length += 2;   /* TRAPcc.W */
                else if (reg == 3) d.length += 4;   /* TRAPcc.L */
                /* reg == 4: no operand, 2 bytes total. */
                break;
            }
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
            /* M7.5f — 68020+ PACK (line 8) / UNPK (line C). Both have a
             * 16-bit adjustment word following the opcode (no EA bytes
             * because operands are register pair or pre-decrement pair). */
            if (top == 0x8 && (op & 0x01F0) == 0x0140) {
                d.length += 2; break;            /* PACK */
            }
            if (top == 0xC && (op & 0x01F0) == 0x0180) {
                d.length += 2; break;            /* UNPK */
            }
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
            /* M7.5d — 68020+ bitfield ops (BFTST/BFEXTU/BFCHG/BFEXTS/BFCLR/
             * BFFFO/BFSET/BFINS): mask 0xF8C0 == 0xE8C0. 4-byte minimum
             * (op + 2-byte ext word); the EA contributes its usual bytes
             * for memory modes. The default szf==3 below would size
             * these wrong (no ext word counted). */
            if ((op & 0xF8C0) == 0xE8C0) {
                d.length += 2;                              /* bitfield ext word */
                if (mode != 0) {
                    d.length += ea_ext_bytes(cpu, pc + 2, mode, reg, 4);
                }
                break;
            }
            if (szf == 3) d.length += ea_ext_bytes(cpu, pc, mode, reg, 2);
            break;
        }
        /* line-A (Toolbox trap) — exception, ends block. */
        case 0xA:
            d.ends_block = true;
            break;
        /* line-F — usually trap (ends block), but 68020+ PMMU (cp-id 0)
         * and CINV/CPUSH cache control are NOT traps and shouldn't end
         * the block. M7.5g — let the JIT keep them in-block under SE/30
         * mode. */
        case 0xF:
            if ((op & 0xFF20) == 0xF400 || (op & 0xFF20) == 0xF420) {
                /* CINV / CPUSH — 2 bytes, no operand. */
                break;
            }
            if ((op & 0xFE00) == 0xF000) {
                /* PMMU (cp-id 0): 4 bytes (op + ext) plus EA bytes for
                 * memory-mode PMOVE. */
                d.length += 2;
                if (mode != 0 && mode != 1) {
                    d.length += ea_ext_bytes(cpu, pc + 2, mode, reg, 4);
                }
                break;
            }
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
        /* SE/30 deferred bus error — mac_read / mac_write set this when
         * an unmapped address is touched, so the ROM's BERR-recovery
         * mechanism (handler at vec 2 checks D7 bit 27, jumps via A6)
         * fires after the offending instruction. */
        if (cpu->bus_error_pending) {
            cpu->fault_addr = cpu->bus_error_pending & 0x0FFFFFFFu;
            cpu->bus_error_pending = 0;
            m68k_exception(cpu, 2);
        }
    }
}
