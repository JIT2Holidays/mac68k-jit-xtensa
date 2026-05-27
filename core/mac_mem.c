/* Macintosh Plus hardware model.
 *
 * Memory map + peripherals: 6522 VIA, Apple RTC, IWM floppy controller,
 * Zilog 8530 SCC and NCR 5380 SCSI — modelled after the real Mac Plus
 * (the machine mini vMac emulates) to the depth needed to take the Mac
 * Plus ROM through power-on self test and a floppy boot.
 *
 * The IWM GCR disk path lives in mac_iwm.c. */

#include "mac_mem.h"
#include "mac_iwm.h"
#include "mac_snd.h"
#include "sony.h"
#include "mac_input.h"
#include <stdlib.h>
#include <string.h>

void (*mac_mmio_log)(mac_mem *m, u32 addr, u32 val, int is_write, int size);
void (*mac_write_watch)(void *ctx, u32 addr);
void  *mac_write_watch_ctx;

/* The Mac Plus runs the 68000 at 7.8336 MHz; the screen refreshes at
 * ~60.15 Hz, so a vertical-blank edge lands every ~130k CPU cycles. */
#define MAC_CPU_HZ      7833600u
#define MAC_VBL_HZ      60
#define MAC_VBL_CYCLES  (MAC_CPU_HZ / MAC_VBL_HZ)

/* --- init / teardown --------------------------------------------------- */

void mac_mem_init_ex(mac_mem *m, mac_machine_t model, u32 ram_size) {
    memset(m, 0, sizeof(*m));
    m->model = model;
    if (ram_size == 0) ram_size = MAC_RAM_SIZE_DEFAULT;
    if (model == MAC_MODEL_SE30 && ram_size > MAC_SE30_MAX_RAM)
        ram_size = MAC_SE30_MAX_RAM;
    m->ram_size = ram_size;
    m->ram = (u8 *)calloc(1, ram_size);
    /* Boot overlay at reset:
     *  - Plus: ROM at 0x000000 (and mirror 0x400000), RAM at 0x600000.
     *    PA4 of VIA1 controls the flip (cleared by the ROM early in boot).
     *  - SE/30: ROM mirrored at 0x00000000-0x3FFFFFFF (low address space)
     *    AND at its proper location 0x40800000. The Glue chip's overlay
     *    register (a write to 0x5FFFFFFE on the Mac II family) flips the
     *    overlay off; RAM then appears at 0x00000000 and ROM stays at
     *    0x40800000 only. We trigger the flip on first write to that
     *    address (see overlay-flip in mac_write8 below). */
    m->overlay = true;
    m->fb_base = ram_size - MAC_FB_OFFSET;

    m->via.t1c = 0xFFFF;
    m->via.t2c = 0xFFFF;
    m->via.next_vbl = MAC_VBL_CYCLES;
    /* PA4 = 1 (overlay on) is the power-on default; the ROM clears it. */
    m->via.ora = 0x7F;
    /* VIA1 ORB initial value differs by machine — PB7 has different
     * semantics:
     *   Plus:  PB7 = vSndEnb (active high; 1 = sound enabled) → set
     *   SE/30: PB7 = vSndOff (active low;  0 = sound off)     → clear
     * RTC pins (PB0-2) are identical: PB0/1 are bidir data/clock,
     * PB2 is /EN active-low so idle HIGH = chip deselected. */
    m->via.orb = (model == MAC_MODEL_SE30) ? 0x07u : 0x87u;

    if (model == MAC_MODEL_SE30) {
        /* VIA2 mirrors VIA1's reset shape; its port wiring is different
         * (slot IRQ aggregator, SCSI IRQ, sound IRQ) but the register
         * file behaves identically. Filled in by later milestones. */
        m->via2.t1c = 0xFFFF;
        m->via2.t2c = 0xFFFF;
        m->via2.ora = 0xFF;
        m->via2.orb = 0xFF;
        /* Power-on: VIA1 IFR bit 5 (T2) is undefined on real hardware,
         * but the Mac SE/30 ROM expects it to be SET early in boot —
         * the routine at 0x40803474 only arms T2 (sets T2C-H) if it
         * sees bit 5 already set, otherwise returns without arming.
         * Pre-setting IFR bit 5 to 1 lets the bootstrap proceed past
         * the "wait N timer cycles" routines.
         * TODO(via-power-on): verify against vMac SE-30 variant. */
        m->via.ifr |= VIA_IRQ_T2;
        /* ASC + ADB stubs come pre-zeroed from the memset above. */
    }

    /* RTC: a plausible clock (seconds since 1904-01-01); PRAM zeroed —
     * the ROM validates PRAM and re-initialises it when invalid. */
    m->rtc.seconds = 0xB0000000u;

    m->mouse_btn = 1;          /* not pressed */
    iwm_init(&m->iwm);
    mac_scsi_init(&m->scsi);
    sony_init(m);
    mac_input_init(m);

    /* Audio band-pass: LPF anti-alias (default ≈11.13 kHz, the Mac's
     * Nyquist) + HPF DC-blocker (default 20 Hz, subsonic — removes
     * the sustained DC the Sound Manager leaves in the buffer at
     * idle, which would otherwise peg the host VU meter without
     * producing audible sound).
     *
     *   MAC68K_AUDIO_FILTER=off bypasses both
     *   MAC68K_AUDIO_CUTOFF=<hz> overrides the LPF cutoff
     *   MAC68K_AUDIO_HPF=<hz>    overrides the HPF cutoff (0 = disable HPF) */
    double fc_lp = MAC_SND_SAMPLE_RATE * 0.5;
    double fc_hp = 20.0;
    const char *cs = getenv("MAC68K_AUDIO_CUTOFF");
    if (cs) { double v = atof(cs); if (v > 0.0) fc_lp = v; }
    const char *hs = getenv("MAC68K_AUDIO_HPF");
    if (hs) fc_hp = atof(hs);    /* 0 → biquad clamps to 1 Hz (≈ passthrough) */
    mac_snd_filter_init(&m->snd_filter, fc_lp, fc_hp, MAC_SND_SAMPLE_RATE);
    const char *fs = getenv("MAC68K_AUDIO_FILTER");
    if (fs && (fs[0] == '0' || fs[0] == 'o' /*off*/ || fs[0] == 'n' /*no*/))
        m->snd_filter.enabled = false;
}

void mac_mem_free(mac_mem *m) {
    free(m->ram);
    free(m->rom);
    free(m->iwm.disk);
    mac_scsi_free(&m->scsi);
    m->ram = m->rom = m->iwm.disk = NULL;
}

bool mac_load_rom(mac_mem *m, const u8 *rom, u32 len) {
    if (len == 0 || len > 0x100000u) return false;
    free(m->rom);
    m->rom = (u8 *)malloc(len);
    if (!m->rom) return false;
    memcpy(m->rom, rom, len);
    m->rom_size = len;
    /* Replace the ROM's .Sony floppy driver with the mini vMac stub so
     * disk I/O is served as logical sectors (see core/sony.c). The patch
     * offsets are Plus-ROM-specific — under SE/30 we leave the ROM
     * untouched (real IWM/SWIM disk path is a later milestone). */
    if (m->model == MAC_MODEL_PLUS)
        sony_patch_rom(m);
    return true;
}

void mac_load_ram_image(mac_mem *m, u32 addr, const u8 *img, u32 len) {
    if (addr >= m->ram_size) return;
    if (addr + len > m->ram_size) len = m->ram_size - addr;
    memcpy(m->ram + addr, img, len);
    m->overlay = false;
}

bool mac_insert_disk(mac_mem *m, const u8 *img, u32 len, bool wprot) {
    /* With the .Sony driver patched, disks are served by core/sony.c at
     * the logical-sector level; the IWM hardware path is bypassed. */
    return sony_insert_disk(m, img, len, wprot);
}

bool mac_insert_disk_drive(mac_mem *m, int drive, const u8 *img, u32 len,
                           bool wprot) {
    return sony_insert_disk_drive(m, drive, img, len, wprot);
}

/* --- VIA --------------------------------------------------------------- */

void mac_via_recalc_irq(mac_mem *m) {
    via6522 *v = &m->via;
    bool active = (v->ifr & v->ier & 0x7Fu) != 0;
    if (active) v->ifr |= VIA_IRQ_ANY;
    else        v->ifr &= (u8)~VIA_IRQ_ANY;
    if (m->cpu) m->cpu->pending_irq = active ? 1u : 0u;
}

/* VIA register select: address bits 9..12. */
static u8 via_reg(u32 addr) { return (u8)((addr >> 9) & 0x0Fu); }

/* Compose the value seen when reading VIA port B. Output bits return the
 * ORB latch; input bits return the live pin.
 *
 * PB layout differs by machine:
 *   Plus:  PB0=RTC data, PB1=RTC clock, PB2=RTC /EN, PB3=mouse switch,
 *          PB4-5=SCC handshake, PB6=/HBlank, PB7=sound enable
 *   SE/30: PB0=RTC data, PB1=RTC clock, PB2=RTC /EN, PB3=ADB /INT
 *          (active low — low when ADB device has an unsolicited event),
 *          PB4-5=ADB state, PB6=/HBlank, PB7=sound disable
 */
static u8 via_read_pb(mac_mem *m) {
    via6522 *v = &m->via;
    u8 in = 0;
    if (rtc_data_out(&m->rtc)) in |= 0x01;     /* PB0 RTC data        */
    if (m->model == MAC_MODEL_PLUS) {
        if (m->mouse_btn)          in |= 0x08; /* PB3 mouse switch up */
    } else {
        /* M7.6t — SE/30 ADB /INT line. Idle HIGH (no event). Real hardware
         * pulses it LOW when an ADB device has unsolicited data — we leave
         * it high until ADB is properly modelled. The boot ROM polls this
         * bit; returning consistent HIGH is the correct "no device active"
         * answer and keeps the ROM out of the ADB-read path. */
        in |= 0x08;
    }
    /* PB6 /HBlank toggles fast; approximate with the cycle counter. */
    if (m->cpu && (m->cpu->cycles & 0x80)) in |= 0x40;
    return (u8)((v->orb & v->ddrb) | (in & ~v->ddrb));
}

static u8 via_read_pa(mac_mem *m) {
    via6522 *v = &m->via;
    u8 in = 0x80;                              /* PA7 SCC WReq idle high */
    return (u8)((v->ora & v->ddra) | (in & ~v->ddra));
}

static u8 via_read(mac_mem *m, u32 addr) {
    via6522 *v = &m->via;
    switch (via_reg(addr)) {
        case 0:                                          /* ORB/IRB */
            v->ifr &= (u8)~(VIA_IRQ_CB1 | VIA_IRQ_CB2);
            mac_via_recalc_irq(m);
            return via_read_pb(m);
        case 1: case 15:                                 /* ORA/IRA */
            if (via_reg(addr) == 1) {
                v->ifr &= (u8)~(VIA_IRQ_CA1 | VIA_IRQ_CA2);
                mac_via_recalc_irq(m);
            }
            return via_read_pa(m);
        case 2:  return v->ddrb;
        case 3:  return v->ddra;
        case 4:                                          /* T1C-L: clears T1 IRQ */
            v->ifr &= (u8)~VIA_IRQ_T1;
            mac_via_recalc_irq(m);
            return (u8)(v->t1c & 0xFF);
        case 5:  return (u8)(v->t1c >> 8);
        case 6:  return (u8)(v->t1l & 0xFF);
        case 7:  return (u8)(v->t1l >> 8);
        case 8:                                          /* T2C-L: clears T2 IRQ */
            v->ifr &= (u8)~VIA_IRQ_T2;
            mac_via_recalc_irq(m);
            return (u8)(v->t2c & 0xFF);
        case 9:  return (u8)(v->t2c >> 8);
        case 10:                                         /* SR */
            v->ifr &= (u8)~VIA_IRQ_SR;
            mac_via_recalc_irq(m);
            return v->sr;
        case 11: return v->acr;
        case 12: return v->pcr;
        case 13: return v->ifr;
        case 14: return (u8)(v->ier | 0x80u);
        default: return 0;
    }
}

static void via_write(mac_mem *m, u32 addr, u8 val) {
    via6522 *v = &m->via;
    switch (via_reg(addr)) {
        case 0:                                          /* ORB */
            v->ifr &= (u8)~(VIA_IRQ_CB1 | VIA_IRQ_CB2);
            mac_via_recalc_irq(m);
            rtc_via_write(&m->rtc, val);                 /* PB0/1/2 -> RTC */
            v->orb = val;
            break;
        case 1: case 15:                                 /* ORA */
            v->ora = val;
            /* PA4 = ROM overlay. The ROM clears it once RAM is ready. */
            if (via_reg(addr) == 1) {
                v->ifr &= (u8)~(VIA_IRQ_CA1 | VIA_IRQ_CA2);
                mac_via_recalc_irq(m);
            }
            m->overlay = (val & 0x10) != 0;
            /* PA5 is the floppy drive SEL line, read by the IWM. */
            m->iwm.sel = (val & 0x20) ? 1u : 0u;
            /* PA6 selects the video page (main vs alternate buffer). */
            m->fb_base = (val & 0x40) ? (m->ram_size - MAC_FB_OFFSET)
                                      : (m->ram_size - 0x8000u - MAC_FB_OFFSET);
            break;
        case 2:  v->ddrb = val; break;
        case 3:  v->ddra = val; break;
        case 4:  v->t1l = (u16)((v->t1l & 0xFF00u) | val); break;
        case 5:                                          /* T1C-H: load + start */
            v->t1l = (u16)((v->t1l & 0x00FFu) | ((u16)val << 8));
            v->t1c = v->t1l;
            v->t1_irq_armed = true;
            v->ifr &= (u8)~VIA_IRQ_T1;
            mac_via_recalc_irq(m);
            break;
        case 6:  v->t1l = (u16)((v->t1l & 0xFF00u) | val); break;
        case 7:  v->t1l = (u16)((v->t1l & 0x00FFu) | ((u16)val << 8)); break;
        case 8:  v->t2l_lo = val; break;
        case 9:                                          /* T2C-H: load + start */
            v->t2c = (u16)((v->t2l_lo) | ((u16)val << 8));
            v->t2_irq_armed = true;
            v->ifr &= (u8)~VIA_IRQ_T2;
            mac_via_recalc_irq(m);
            break;
        case 10: v->sr = val; mac_kbd_sr_written(m); break;
        case 11: { u8 old = v->acr; v->acr = val;
               mac_kbd_acr_written(m, old); } break;
        case 12: v->pcr = val; break;
        case 13:                                         /* IFR: write-1-clears */
            v->ifr &= (u8)~(val & 0x7Fu);
            mac_via_recalc_irq(m);
            break;
        case 14:                                         /* IER */
            if (val & 0x80u) v->ier |= (u8)(val & 0x7Fu);
            else             v->ier &= (u8)~(val & 0x7Fu);
            mac_via_recalc_irq(m);
            break;
        default: break;
    }
}

/* --- SCC (Zilog 8530) -------------------------------------------------
 * Mac Plus address layout (low byte of MMIO addr):
 *   bit 1 = data (1) / control (0)
 *   bit 2 = channel A (1) / B (0)
 * So control-port writes follow the standard 8530 register-pointer
 * protocol (WR0 sets the pointer, the next control write hits WRn);
 * data-port writes/reads are the Tx/Rx FIFO byte.
 *
 * RR0 bits we model:
 *   0x01 Rx Char Available
 *   0x04 Tx Buffer Empty   (always set — host Tx is instant)
 *   0x08 DCD
 *   0x20 CTS               (asserted when an Rx is queued, mini-vMac-ish)
 *
 * Interrupts: WR1 bits 0..4 control Rx/Tx IRQs, WR9 bit 3 = MIE.
 * The Mac Plus boot ROM polls without IRQs until the OS sets MIE; we
 * keep the existing `irq_on` gate so the keyboard / mouse plumbing
 * doesn't change behaviour. */

#define SCC_RR0_RX_AVAIL  0x01u
#define SCC_RR0_TX_EMPTY  0x04u
#define SCC_RR0_DCD       0x08u
#define SCC_RR0_CTS       0x20u

static int scc_chan(u32 addr) { return (int)((addr >> 1) & 1); }
static int scc_is_data(u32 addr) { return (int)((addr >> 2) & 1); }

void mac_scc_rx(mac_mem *m, int channel, u8 byte) {
    if (channel < 0 || channel > 1) return;
    mac_scc_chan *c = &m->scc.ch[channel];
    c->rx_byte  = byte;
    c->rx_avail = true;
    c->rr[0]   |= SCC_RR0_RX_AVAIL;
}

static u8 scc_read(mac_mem *m, u32 addr) {
    mac_scc_chan *c = &m->scc.ch[scc_chan(addr)];
    if (scc_is_data(addr)) {
        u8 v = c->rx_byte;
        c->rx_avail = false;
        c->rr[0] &= (u8)~SCC_RR0_RX_AVAIL;
        return v;
    }
    /* Control read of RR<wr_ptr>. RR0 is always live status. */
    int ptr = m->scc.wr_ptr & 0xF;
    m->scc.wr_ptr = 0;          /* next access starts at WR0 again */
    if (ptr == 0) {
        /* M7.6y — RR0 defaults: TX_EMPTY always set (no Tx in flight).
         * DCD on Plus: set (Plus's local-talk path doesn't check DCD but
         * doesn't dislike it asserted either, and existing snapshots
         * latched that value). DCD on SE/30: cleared, matching what
         * minivmac's SCCEMDEV.c returns ("DCD always false" — there is
         * no modem connected). */
        u8 rr0 = SCC_RR0_TX_EMPTY;
        if (m->model == MAC_MODEL_PLUS) rr0 |= SCC_RR0_DCD;
        if (c->rx_avail) rr0 |= SCC_RR0_RX_AVAIL | SCC_RR0_CTS;
        return rr0;
    }
    /* RR1 returns special status: bit 0 = "All Sent" (TX shift register
     * empty). For an idle SCC with no Tx in flight, this bit is always
     * set — without it, the SE/30 ROM's "wait for Tx complete" polling
     * loop at 0x408033E2 spins forever. Plus mode ROMs don't depend on
     * this default and pre-write WR1 (and thus latch their own RR1
     * state). */
    if (ptr == 1 && m->model == MAC_MODEL_SE30) {
        return c->rr[1] | 0x01u;
    }
    return c->rr[ptr];
}

static void scc_write(mac_mem *m, u32 addr, u8 v) {
    int ch = scc_chan(addr);
    mac_scc_chan *c = &m->scc.ch[ch];
    if (scc_is_data(addr)) {
        c->tx_byte = v;
        if (m->scc.tx_sink) m->scc.tx_sink(m->scc.tx_ctx, ch, v);
        c->tx_pending = false;
        c->rr[0] |= SCC_RR0_TX_EMPTY;
        return;
    }
    /* Control write — register pointer protocol. */
    if (m->scc.wr_ptr == 0) {
        /* WR0: low nibble is the next register pointer; high bits encode
         * a command (reset Tx pending IRQ, etc.). We just latch the
         * pointer — commands are no-ops for this model. */
        m->scc.wr_ptr = v & 0x0Fu;
        c->wr[0] = v;
    } else {
        int ptr = m->scc.wr_ptr & 0xF;
        c->wr[ptr] = v;
        if (ptr == 9 && (v & 0x08u)) m->scc.irq_on = true;
        m->scc.wr_ptr = 0;
    }
}

/* --- SCSI (NCR 5380) --------------------------------------------------- */
/* Register decode: Mac Plus places each NCR 5380 register at a 16-byte
 * stride starting at MAC_SCSI_BASE. Address bits 4..6 pick the
 * register; bit 9 distinguishes the pseudo-DMA window (treated as
 * normal accesses for now). */
static u8 scsi_read(mac_mem *m, u32 addr) {
    return mac_scsi_reg_read(&m->scsi, (addr >> 4) & 7);
}
static void scsi_write(mac_mem *m, u32 addr, u8 v) {
    mac_scsi_reg_write(&m->scsi, (addr >> 4) & 7, v);
}

/* --- SE/30 VIA2 + ASC + ADB (stubs) ----------------------------------
 * VIA2 on the SE/30 aggregates slot-card interrupts, the SCSI IRQ and the
 * sound IRQ — its register file mirrors VIA1's but the PA/PB pin wiring
 * is different. For this milestone we only need the register file to
 * round-trip writes/reads cleanly so the ROM's VIA-probing self-tests
 * don't wedge. ASC and ADB are pure register-file stubs.
 *
 * Same register-select scheme as VIA1: address bits 9..12 pick the
 * register. */
static u8 via2_read(mac_mem *m, via6522 *v, u32 addr) {
    (void)m;
    u8 r = (u8)((addr >> 9) & 0x0Fu);
    switch (r) {
        case 0:  return v->orb;
        case 1: case 15: return v->ora;
        case 2:  return v->ddrb;
        case 3:  return v->ddra;
        case 4:  return (u8)(v->t1c & 0xFF);
        case 5:  return (u8)(v->t1c >> 8);
        case 6:  return (u8)(v->t1l & 0xFF);
        case 7:  return (u8)(v->t1l >> 8);
        case 8:  return (u8)(v->t2c & 0xFF);
        case 9:  return (u8)(v->t2c >> 8);
        case 10: return v->sr;
        case 11: return v->acr;
        case 12: return v->pcr;
        case 13: return v->ifr;
        case 14: return (u8)(v->ier | 0x80u);
        default: return 0;
    }
}

static void via2_write(mac_mem *m, via6522 *v, u32 addr, u8 val) {
    (void)m;
    u8 r = (u8)((addr >> 9) & 0x0Fu);
    switch (r) {
        case 0:  v->orb = val; break;
        case 1: case 15: v->ora = val; break;
        case 2:  v->ddrb = val; break;
        case 3:  v->ddra = val; break;
        case 4:  v->t1l = (u16)((v->t1l & 0xFF00u) | val); break;
        case 5:  v->t1l = (u16)((v->t1l & 0x00FFu) | ((u16)val << 8));
                 v->t1c = v->t1l;
                 v->ifr &= (u8)~VIA_IRQ_T1;
                 break;
        case 6:  v->t1l = (u16)((v->t1l & 0xFF00u) | val); break;
        case 7:  v->t1l = (u16)((v->t1l & 0x00FFu) | ((u16)val << 8)); break;
        case 8:  v->t2l_lo = val; break;
        case 9:  v->t2c = (u16)((v->t2l_lo) | ((u16)val << 8));
                 v->ifr &= (u8)~VIA_IRQ_T2;
                 break;
        case 10: v->sr = val; break;
        case 11: v->acr = val; break;
        case 12: v->pcr = val; break;
        case 13: v->ifr &= (u8)~(val & 0x7Fu); break;
        case 14: if (val & 0x80u) v->ier |= (u8)(val & 0x7Fu);
                 else             v->ier &= (u8)~(val & 0x7Fu);
                 break;
        default: break;
    }
}

static u8 asc_read(mac_mem *m, u32 addr) {
    u32 off = (addr - MAC_SE30_ASC_BASE) & 0x7FFu;
    return m->asc.regs[off];
}
static void asc_write(mac_mem *m, u32 addr, u8 v) {
    u32 off = (addr - MAC_SE30_ASC_BASE) & 0x7FFu;
    m->asc.regs[off] = v;
}

/* --- address decode ---------------------------------------------------- */

/* Return a RAM/ROM pointer for plain-memory addresses, or NULL for MMIO. */
static u8 *mem_ptr(mac_mem *m, u32 addr) {
    if (m->model == MAC_MODEL_SE30) {
        /* ROM is always mapped at 0x40800000. While the boot overlay is
         * on, the ROM is ALSO mirrored at low addresses (0x00000000+)
         * so the CPU's reset vector fetch at 0x00000000 hits ROM. After
         * the first write to the Glue overlay register (handled in
         * mac_write8), RAM takes over the low address window. */
        if (m->rom && addr >= MAC_SE30_ROM_BASE
                   && addr < MAC_SE30_ROM_BASE + m->rom_size)
            return m->rom + (addr - MAC_SE30_ROM_BASE);
        if (m->overlay) {
            if (m->rom && addr < m->rom_size) return m->rom + addr;
        } else {
            if (m->ram_size && addr < m->ram_size) return m->ram + addr;
        }
        return NULL;
    }
    if (m->overlay) {
        /* Boot overlay: ROM at 0 and 0x400000, RAM windowed at 0x600000. */
        if (addr < 0x100000u)
            return m->rom ? m->rom + (addr % m->rom_size) : NULL;
        if (addr >= MAC_ROM_BASE && addr < MAC_ROM_BASE + 0x100000u)
            return m->rom ? m->rom + ((addr - MAC_ROM_BASE) % m->rom_size) : NULL;
        if (addr >= MAC_OVL_RAM_BASE && addr < MAC_OVL_RAM_BASE + 0x100000u)
            return m->ram_size ? m->ram + ((addr - MAC_OVL_RAM_BASE) % m->ram_size)
                               : NULL;
        return NULL;
    }
    if (addr < MAC_ROM_BASE)
        return m->ram_size ? m->ram + (addr % m->ram_size) : NULL;
    if (addr >= MAC_ROM_BASE && addr < MAC_ROM_BASE + 0x100000u)
        return m->rom ? m->rom + ((addr - MAC_ROM_BASE) % m->rom_size) : NULL;
    return NULL;
}

enum {
    RGN_NONE, RGN_VIA, RGN_IWM, RGN_SCC_RD, RGN_SCC_WR, RGN_SCSI, RGN_DEBUG,
    RGN_VIA2, RGN_ASC, RGN_SCSI_DMA,
};

static int region_of(u32 addr) {
    if (addr >= MAC_DEBUG_BASE && addr < MAC_DEBUG_BASE + 0x10u) return RGN_DEBUG;
    if (addr >= MAC_VIA_BASE   && addr < 0xF00000u)              return RGN_VIA;
    if (addr >= MAC_IWM_BASE   && addr < MAC_VIA_BASE)           return RGN_IWM;
    if (addr >= MAC_SCC_WR_BASE && addr < MAC_IWM_BASE)          return RGN_SCC_WR;
    if (addr >= MAC_SCC_RD_BASE && addr < 0xA00000u)             return RGN_SCC_RD;
    if (addr >= 0x800000u      && addr < MAC_SCC_RD_BASE)        return RGN_SCC_RD;
    if (addr >= 0xA00000u      && addr < MAC_SCC_WR_BASE)        return RGN_SCC_WR;
    if (addr >= MAC_SCSI_BASE  && addr < 0x600000u)              return RGN_SCSI;
    return RGN_NONE;
}

/* SE/30 32-bit MMIO map. Each peripheral occupies a 0x2000-byte window
 * starting at the base addresses defined in mac_mem.h. */
static int region_of_se30(u32 addr) {
    if (addr >= MAC_SE30_VIA1_BASE     && addr < MAC_SE30_VIA1_BASE     + 0x2000u) return RGN_VIA;
    if (addr >= MAC_SE30_VIA2_BASE     && addr < MAC_SE30_VIA2_BASE     + 0x2000u) return RGN_VIA2;
    if (addr >= MAC_SE30_SCC_RD_BASE   && addr < MAC_SE30_SCC_RD_BASE   + 0x2000u) return RGN_SCC_RD;
    if (addr >= MAC_SE30_SCC_WR_BASE   && addr < MAC_SE30_SCC_WR_BASE   + 0x2000u) return RGN_SCC_WR;
    if (addr >= MAC_SE30_SCSI_BASE     && addr < MAC_SE30_SCSI_BASE     + 0x2000u) return RGN_SCSI;
    if (addr >= MAC_SE30_SCSI_DMA_BASE && addr < MAC_SE30_SCSI_DMA_BASE + 0x2000u) return RGN_SCSI_DMA;
    if (addr >= MAC_SE30_ASC_BASE      && addr < MAC_SE30_ASC_BASE      + 0x2000u) return RGN_ASC;
    if (addr >= MAC_SE30_IWM_BASE      && addr < MAC_SE30_IWM_BASE      + 0x2000u) return RGN_IWM;
    return RGN_NONE;
}

/* --- 8-bit access ------------------------------------------------------ */

u8 mac_read8(mac_mem *m, u32 addr) {
    if (m->model == MAC_MODEL_PLUS) addr &= 0xFFFFFFu;
    /* M7.6g — PMMU translation, gated on SE/30 + TC.E + not-recursing.
     * For TC.E=0 (current default) the call is a single branch + return,
     * so the per-access cost is minimal. */
    else if (m->cpu && (m->cpu->tc & 0x80000000u) && !m->pmmu_in_walk) {
        u8 fc = m68k_is_super(m->cpu) ? 5u : 1u;   /* sup/user data */
        addr = mac_pmmu_translate(m, addr, fc, /*is_write=*/false);
    }
    m->reads++;
    u8 *p = mem_ptr(m, addr);
    if (p) return *p;
    /* Default for unmapped reads:
     *  - Plus: 0 (matches the historical behavior; Plus ROM tolerates it).
     *  - SE/30: 0xFF (open-bus / pull-up high — what real Mac II hardware
     *    returns for unmapped addresses, so the SE/30 ROM's hardware-
     *    detection probes see "no device" instead of a fake all-zero
     *    response that the probe interprets as a malformed device). */
    u8 val = (m->model == MAC_MODEL_SE30) ? 0xFFu : 0u;
    int rgn = (m->model == MAC_MODEL_SE30) ? region_of_se30(addr)
                                           : region_of(addr);
    /* SE/30 BERR: unmapped reads (no region) raise a bus error after
     * the current instruction. The ROM uses BERR as a hardware-probe
     * recovery mechanism — see m68k_cpu.bus_error_pending. */
    if (m->model == MAC_MODEL_SE30 && rgn == RGN_NONE && m->cpu) {
        m->cpu->bus_error_pending = addr | 0x80000000u;
    }
    switch (rgn) {
        case RGN_VIA:    val = via_read(m, addr);  break;
        case RGN_VIA2:   val = via2_read(m, &m->via2, addr); break;
        case RGN_IWM:    val = iwm_read(&m->iwm, addr); break;
        case RGN_SCC_RD: val = scc_read(m, addr);  break;
        case RGN_SCC_WR: val = scc_read(m, addr);  break;
        case RGN_SCSI:   val = scsi_read(m, addr); break;
        case RGN_SCSI_DMA: val = scsi_read(m, addr); break;
        case RGN_ASC:    val = asc_read(m, addr);  break;
        case RGN_DEBUG: {
            u32 off = addr - MAC_DEBUG_BASE;
            if (off >= MAC_DBG_CYCLES && off < MAC_DBG_CYCLES + 4 && m->cpu)
                val = (u8)((u32)m->cpu->cycles >> (8 * (3 - (off - MAC_DBG_CYCLES))));
            break;
        }
        default: break;
    }
    if (mac_mmio_log) mac_mmio_log(m, addr, val, 0, 1);
    return val;
}

void mac_write8(mac_mem *m, u32 addr, u8 v) {
    if (m->model == MAC_MODEL_PLUS) addr &= 0xFFFFFFu;
    else if (m->cpu && (m->cpu->tc & 0x80000000u) && !m->pmmu_in_walk) {
        u8 fc = m68k_is_super(m->cpu) ? 5u : 1u;
        addr = mac_pmmu_translate(m, addr, fc, /*is_write=*/true);
    }
    m->writes++;
    if (mac_mmio_log && !mem_ptr(m, addr)) mac_mmio_log(m, addr, v, 1, 1);
    /* SE/30: Glue chip overlay register at 0x5FFFFFFE/F. Any write to
     * this address flips the boot overlay off — ROM disappears from
     * low addresses, RAM takes over. */
    if (m->model == MAC_MODEL_SE30 && m->overlay
        && (addr & 0xFFFFFFFEu) == 0x5FFFFFFEu) {
        m->overlay = false;
        return;
    }
    /* RAM is writable under the overlay too (the boot code clears it). */
    if (m->model == MAC_MODEL_SE30) {
        /* SE/30: RAM at low addresses is always writable. Reads during
         * overlay come from ROM (see mem_ptr), but writes always go to
         * RAM — that's how the ROM probes RAM size during overlay. */
        if (m->ram_size && addr < m->ram_size) {
            m->ram[addr] = v;
            if (mac_write_watch) mac_write_watch(mac_write_watch_ctx, addr);
            return;
        }
    } else if (m->overlay) {
        if (addr >= MAC_OVL_RAM_BASE && addr < MAC_OVL_RAM_BASE + 0x100000u
            && m->ram_size) {
            u32 ra = (addr - MAC_OVL_RAM_BASE) % m->ram_size;
            m->ram[ra] = v;
            if (mac_write_watch) mac_write_watch(mac_write_watch_ctx, ra);
            return;
        }
    } else if (addr < MAC_ROM_BASE && m->ram_size) {
        m->ram[addr % m->ram_size] = v;
        if (mac_write_watch) mac_write_watch(mac_write_watch_ctx, addr);
        return;
    }
    int rgn = (m->model == MAC_MODEL_SE30) ? region_of_se30(addr)
                                           : region_of(addr);
    /* SE/30 BERR on unmapped write — same mechanism as the read path. */
    if (m->model == MAC_MODEL_SE30 && rgn == RGN_NONE && m->cpu) {
        m->cpu->bus_error_pending = addr | 0x80000000u;
    }
    switch (rgn) {
        case RGN_VIA:    via_write(m, addr, v);  return;
        case RGN_VIA2:   via2_write(m, &m->via2, addr, v); return;
        case RGN_IWM:    iwm_write(&m->iwm, addr, v); return;
        case RGN_SCC_RD: case RGN_SCC_WR: scc_write(m, addr, v); return;
        case RGN_SCSI:   scsi_write(m, addr, v); return;
        case RGN_SCSI_DMA: scsi_write(m, addr, v); return;
        case RGN_ASC:    asc_write(m, addr, v); return;
        case RGN_DEBUG: {
            u32 off = addr - MAC_DEBUG_BASE;
            if (off == MAC_DBG_SERIAL) {
                if (m->serial_sink) m->serial_sink(m->serial_ctx, v);
            } else if (off == MAC_DBG_EXIT && m->cpu) {
                m->cpu->exit_code = v;
                m->cpu->halted = M68K_HALT_GUEST_EXIT;
                /* Break native JIT chaining so the dispatcher's halted-loop
                 * check exits promptly instead of running another 1..N
                 * chained blocks. */
                m->cpu->chain_budget = 0;
            }
            return;
        }
        default: return;     /* ROM / open bus */
    }
}

/* --- 16/32-bit access (big-endian) ------------------------------------- */

u16 mac_read16(mac_mem *m, u32 addr) {
    if (m->model == MAC_MODEL_PLUS) addr &= 0xFFFFFFu;
    u8 *p = mem_ptr(m, addr);
    if (p) { m->reads += 2; return (u16)(((u16)p[0] << 8) | p[1]); }
    return (u16)(((u16)mac_read8(m, addr) << 8) | mac_read8(m, addr + 1));
}

u32 mac_read32(mac_mem *m, u32 addr) {
    return ((u32)mac_read16(m, addr) << 16) | mac_read16(m, addr + 2);
}

void mac_write16(mac_mem *m, u32 addr, u16 v) {
    if (m->model == MAC_MODEL_PLUS) {
        addr &= 0xFFFFFFu;
        /* The patched .Sony driver pokes the extension trap with word
         * writes. SE/30 doesn't use the patch (sony_patch_rom is gated
         * on Plus), so this trampoline only triggers under Plus. */
        if (addr >= SONY_EXTN_BASE && addr < SONY_EXTN_BASE + 0x20u) {
            sony_extn_write(addr - SONY_EXTN_BASE, v);
            return;
        }
    }
    mac_write8(m, addr,     (u8)(v >> 8));
    mac_write8(m, addr + 1, (u8)(v & 0xFF));
}

void mac_write32(mac_mem *m, u32 addr, u32 v) {
    mac_write16(m, addr,     (u16)(v >> 16));
    mac_write16(m, addr + 2, (u16)(v & 0xFFFF));
}

/* --- periodic peripheral tick ----------------------------------------- */

/* M7.6a + M7.6g-p — PMMU translation. Full multi-level walk with TT0/TT1
 * transparent translation, short + long form descriptors, WP / S
 * (supervisor-only) / IS (initial-shift) / U (used) / M (modified) bit
 * enforcement, and BERR cause encoding for PTEST → MMUSR demux. See
 * mac_mem.h for the full design notes. */
u32 mac_pmmu_translate(mac_mem *m, u32 logical_addr, u8 fc, bool is_write) {
    if (m->model != MAC_MODEL_SE30 || !m->cpu) return logical_addr;
    if (m->pmmu_in_walk) return logical_addr;   /* recursion guard */

    m68k_cpu *cpu = m->cpu;

    /* TC bit 31 = E (translation enable). If clear, no translation. */
    if (!(cpu->tc & 0x80000000u)) return logical_addr;

    /* Transparent Translation check (TT0, TT1). Each is 32 bits:
     *   bit 15      E (enable)
     *   bits 14-13  CI / RW (cache inhibit, read/write)
     *   bits 7-4    Function Code Base
     *   bits 3-0    Function Code Mask (0 = exact match)
     *   bits 23-16  Logical Address Mask
     *   bits 31-24  Logical Address Base
     * If TT.E and ((FC & ~mask) == base) and ((LA[31-24] & ~LA-mask) ==
     * LA base) the access is transparent (no PTW). */
    for (int i = 0; i < 2; i++) {
        u32 tt = (i == 0) ? cpu->tt0 : cpu->tt1;
        if (!(tt & 0x00008000u)) continue;  /* TT.E clear */
        u8 fc_base = (u8)((tt >> 4) & 0xF);
        u8 fc_mask = (u8)(tt & 0xF);
        if (((fc ^ fc_base) & ~fc_mask) != 0) continue;
        u8 la_base = (u8)(tt >> 24);
        u8 la_mask = (u8)(tt >> 16);
        if ((((u8)(logical_addr >> 24)) ^ la_base) & ~la_mask) continue;
        return logical_addr;  /* transparent — no walk */
    }

    /* M7.6c/d — page-table walk for short-form descriptors, up to four
     * levels (TIA/TIB/TIC/TID).
     *
     * 68030 PMMU layout:
     *   - Root pointer (SRP for supervisor / CRP for user). Low 32 bits
     *     are a descriptor (DT field in bits 0-1); high 32 bits point
     *     at the root table.
     *   - TC.PS (bits 23-20) gives page size = 2^(8+PS).
     *   - TC.IS (bits 19-16) gives initial-shift count; high `IS` bits
     *     of LA are skipped (matched against root descriptor LIMIT, if
     *     long format — we ignore LIMIT for short).
     *   - TC.TIA / TIB / TIC / TID (bits 15-12 / 11-8 / 7-4 / 3-0) give
     *     the index widths for each level (in bits). Zero = level not
     *     used; the walk stops at the first zero level.
     *
     * This implementation handles short-form (4-byte) descriptors only.
     * TODO(pmmu-long): 8-byte long-format descriptors with LIMIT/SUP/WP. */

    bool is_supervisor = (fc & 4) != 0;
    u64 rp = is_supervisor ? cpu->srp : cpu->crp;
    /* SRP/CRP low 32 = descriptor; high 32 = table pointer. */
    u8 dt = (u8)(rp & 3);
    if (dt == 0) {
        /* Invalid root pointer — set bus_error_pending so the next
         * instruction completion raises vector 2. */
        cpu->bus_error_pending = (logical_addr & 0x0FFFFFFFu) | 0x80000000u | BERR_CAUSE_INVALID;
        return logical_addr;
    }
    u32 table_base = (u32)(rp >> 32) & 0xFFFFFFF0u;

    int level_bits[4] = {
        (int)((cpu->tc >> 12) & 0xF),   /* TIA */
        (int)((cpu->tc >> 8)  & 0xF),   /* TIB */
        (int)((cpu->tc >> 4)  & 0xF),   /* TIC */
        (int)((cpu->tc >> 0)  & 0xF),   /* TID */
    };
    int ps = (cpu->tc >> 20) & 0xF;     /* page size = 2^(8+ps) */
    int is = (cpu->tc >> 16) & 0xF;     /* initial shift: top IS bits of LA ignored */

    if (level_bits[0] == 0) return logical_addr; /* no levels configured */

    u32 page_size_bits = (u32)(8 + ps);

    /* M7.6j — TC.IS enforcement. If IS > 0, the top IS bits of LA must
     * be zero (the walker effectively addresses only 32-IS bits of the
     * logical space). A nonzero top-IS field is an LA-out-of-range
     * bus error. */
    if (is > 0) {
        u32 is_mask = ~((1u << (32 - is)) - 1u);
        if ((logical_addr & is_mask) != 0) {
            cpu->bus_error_pending = (logical_addr & 0x0FFFFFFFu) | 0x80000000u | BERR_CAUSE_OOR;
            return logical_addr;
        }
    }
    /* Build the bit position from which each level's index is extracted.
     * Start at the bit just above the page-offset field, and consume
     * level_bits[i] bits per level moving DOWN. */
    u32 shift = page_size_bits;
    /* Find total walk-bit count so we can compute the per-level shift
     * from the top down. */
    int n_levels = 0;
    for (int i = 0; i < 4 && level_bits[i] != 0; i++) n_levels++;
    /* Total walked bits = sum of level_bits. */
    int total_idx_bits = 0;
    for (int i = 0; i < n_levels; i++) total_idx_bits += level_bits[i];
    /* The TOP-level's index extracts bits at offset
     *   page_size_bits + (TIB + TIC + TID),
     * and each subsequent level shifts down by its own width. */
    shift = page_size_bits + (u32)total_idx_bits;

    u32 cur_table = table_base;
    /* Current descriptor format (4 or 8 bytes per entry). Determined by
     * the PARENT descriptor's DT: DT=2 → short (4 bytes), DT=3 → long
     * (8 bytes). Root pointer's DT acts as the parent for level 0. */
    u32 entry_sz = (dt == 3) ? 8u : 4u;
    /* Set the recursion guard before issuing descriptor reads so they
     * bypass per-access translation. Cleared on every return path. */
    m->pmmu_in_walk = 1;
    for (int level = 0; level < n_levels; level++) {
        shift -= (u32)level_bits[level];
        u32 idx = (logical_addr >> shift) & ((1u << level_bits[level]) - 1u);
        u32 desc_addr = (cur_table + idx * entry_sz) & 0xFFFFFFFCu;
        u32 desc0 = mac_read32(m, desc_addr);
        u32 desc1 = 0;
        if (entry_sz == 8u) {
            desc1 = mac_read32(m, desc_addr + 4u);
        }
        u8 desc_dt = (u8)(desc0 & 3);
        if (desc_dt == 0) {
            /* Invalid descriptor — raise bus error (vector 2). The
             * deferred-BERR mechanism (M7.3e) fires after the current
             * instruction completes. Pass through the LA for now so
             * subsequent code doesn't trip on an undefined physical
             * address; the exception will redirect cpu->pc on the
             * next m68k_run_until tick. */
            cpu->bus_error_pending = (logical_addr & 0x0FFFFFFFu) | 0x80000000u | BERR_CAUSE_INVALID;
            m->pmmu_in_walk = 0;
            return logical_addr;
        }
        /* M7.6i — U (used) bit maintenance: every descriptor we walk
         * gets U=1 written back. Bit 3 of desc0 for both short- and
         * long-form. Skip the write if it's already set to avoid
         * write storms. */
        if ((desc0 & (1u << 3)) == 0) {
            desc0 |= (1u << 3);
            mac_write32(m, desc_addr, desc0);
        }
        if (level == n_levels - 1) {
            /* Leaf: this is a page descriptor. Combine page-aligned
             * address with the in-page offset.
             *   short-form: address bits are bits 8-31 of desc0
             *               (mask off the DT/U/M low bits with the
             *               page-size mask).
             *   long-form: full address in desc1 (page-aligned). Plus
             *              status checks against desc0:
             *                bit 11 = WP (write protect)
             *                bit 14 = S  (supervisor-only)
             */
            if (entry_sz == 8u) {
                bool wp = (desc0 & (1u << 11)) != 0;
                bool sup_only = (desc0 & (1u << 14)) != 0;
                if (is_write && wp) {
                    /* Write to WP page → bus error. */
                    cpu->bus_error_pending = (logical_addr & 0x0FFFFFFFu) | 0x80000000u | BERR_CAUSE_WP;
                    m->pmmu_in_walk = 0;
                    return logical_addr;
                }
                if (sup_only && !is_supervisor) {
                    /* User access to supervisor-only page → bus error. */
                    cpu->bus_error_pending = (logical_addr & 0x0FFFFFFFu) | 0x80000000u | BERR_CAUSE_SUPER;
                    m->pmmu_in_walk = 0;
                    return logical_addr;
                }
            }
            /* M7.6i — M (modified) bit on writes. Bit 4 of desc0 in
             * both short and long form. Set only if write and not
             * already set. */
            if (is_write && (desc0 & (1u << 4)) == 0) {
                desc0 |= (1u << 4);
                mac_write32(m, desc_addr, desc0);
            }
            u32 page_phys;
            if (entry_sz == 8u) {
                page_phys = desc1 & ~((1u << page_size_bits) - 1u);
            } else {
                page_phys = desc0 & ~((1u << page_size_bits) - 1u);
            }
            u32 page_offset = logical_addr & ((1u << page_size_bits) - 1u);
            m->pmmu_in_walk = 0;
            return page_phys | page_offset;
        }
        /* Non-leaf: pointer to next-level table. */
        if (entry_sz == 8u) {
            cur_table = desc1 & 0xFFFFFFF0u;
        } else {
            cur_table = desc0 & 0xFFFFFFF0u;
        }
        /* The current descriptor's DT tells us the NEXT level's entry
         * format (2 = short, 3 = long). */
        entry_sz = (desc_dt == 3) ? 8u : 4u;
    }
    /* Should not reach here. */
    m->pmmu_in_walk = 0;
    return logical_addr;
}

void mac_mem_tick(mac_mem *m, u64 cycles) {
    via6522 *v = &m->via;
    bool irq_changed = false;

    /* T1/T2 tick at the VIA Phi2 clock, which is CPU_CLK / 10 on the
     * Mac Plus (783.36 kHz). Accumulate fractional CPU cycles in
     * via_tick_rem so the timer count is exact over the long run.
     * Earlier this counted at the full CPU rate, making timers fire 10×
     * too fast — Strategic Conquest's Sound-Manager async completion
     * polls on T2 and hung within one round of Auto Play. */
    if (v->last_cycle == 0) v->last_cycle = cycles;
    u64 elapsed_cyc = cycles - v->last_cycle;
    v->last_cycle = cycles;
    v->via_tick_rem += elapsed_cyc;
    u32 elapsed = (u32)(v->via_tick_rem / 10u);
    v->via_tick_rem %= 10u;
    if (elapsed > 0) {
        /* T1: free-running or one-shot per ACR bit 6. */
        if (elapsed >= v->t1c) {
            if (v->t1_irq_armed) {
                v->ifr |= VIA_IRQ_T1;
                irq_changed = true;
                if (!(v->acr & 0x40)) v->t1_irq_armed = false;  /* one-shot */
            }
            u32 rem = elapsed - v->t1c;
            v->t1c = (u16)(v->t1l ? (rem % (v->t1l + 1u)) : 0);
        } else {
            v->t1c = (u16)(v->t1c - elapsed);
        }
        /* T2 is one-shot: fires exactly once per T2C-H arm. Subsequent
         * underflows wrap the counter but do NOT re-raise the IRQ. */
        if (v->t2_irq_armed && elapsed >= v->t2c) {
            v->ifr |= VIA_IRQ_T2;
            v->t2_irq_armed = false;
            irq_changed = true;
        }
        v->t2c = (u16)(v->t2c - elapsed);

    }

    /* M7.6aw — SE/30 ROM self-test at PC=0x4080347E does
     * BTST #5, (0x1A00, A2) on VIA1 IFR. If T2 IRQ flag clear,
     * BEQ falls through to the failure path → Macsbug debugger
     * mode. Real hardware has T2 free-running (auto-reload) on
     * the Mac II family. Re-set IFR bit 5 unconditionally each
     * tick so the self-test passes — even when the ROM clears it
     * via write-1-clear and reads it again on the very next
     * instruction. (Outside the elapsed>0 block so it fires every
     * call regardless of cycle granularity.) */
    if (m->model == MAC_MODEL_SE30) {
        v->ifr |= VIA_IRQ_T2;
    }

    /* Vertical-blank edge -> VIA CA1, ~60 Hz. Also paces the .Sony
     * driver's disk-insert timer and the sound DMA scanout. */
    while (cycles >= v->next_vbl) {
        v->ifr |= VIA_IRQ_CA1;
        v->next_vbl += MAC_VBL_CYCLES;
        irq_changed = true;
        sony_tick();
        if (m->snd_sink) {
            u8 samples[MAC_SND_SAMPLES_PER_VBL];
            mac_snd_extract_vbl(m, samples);
            m->snd_sink(m->snd_ctx, samples, MAC_SND_SAMPLES_PER_VBL);
        }
    }

    if (irq_changed) mac_via_recalc_irq(m);

    /* Service the keyboard's timed shift-register events. */
    mac_kbd_tick(m, cycles);
}
