#ifndef MAC_MEM_H
#define MAC_MEM_H

#include "m68k_types.h"
#include "mac_scsi.h"
#include "mac_snd.h"
#include "m68k_cpu.h"

/* Macintosh Plus hardware model — memory map and peripherals.
 *
 * Modelled after the real Mac Plus (the same machine mini vMac emulates):
 * a 68000, 1 MB RAM, a 128 KB ROM, two 6522-style VIA-driven subsystems,
 * the IWM floppy controller, a Zilog 8530 SCC, an NCR 5380 SCSI chip and
 * the Apple RTC. Enough of each is modelled to take the real Mac Plus ROM
 * through its power-on self test and into the screen-drawing / boot-disk
 * search.
 *
 * Memory map (24-bit bus). The ROM "overlay" (VIA PA4) controls the low
 * region at boot:
 *
 *   overlay ON (reset):          overlay OFF (normal):
 *     000000-0FFFFF  ROM           000000-3FFFFF  RAM
 *     400000-4FFFFF  ROM           400000-4FFFFF  ROM
 *     600000-6FFFFF  RAM
 *   both:
 *     580000-5FFFFF  SCSI (NCR 5380)
 *     900000-9FFFFF  SCC read
 *     B00000-BFFFFF  SCC write
 *     C00000-DFFFFF  IWM  (base DFE1FF)
 *     E80000-EFFFFF  VIA  (base EFE1FE)
 *     F00000-F0000F  debug ports (harness extension — not real hardware)
 */

#define MAC_RAM_SIZE_DEFAULT (1u * 1024u * 1024u)
#define MAC_ROM_BASE         0x400000u
#define MAC_OVL_RAM_BASE     0x600000u   /* RAM window while overlay is on */
#define MAC_SCSI_BASE        0x580000u
#define MAC_SCC_RD_BASE      0x900000u
#define MAC_SCC_WR_BASE      0xB00000u
#define MAC_IWM_BASE         0xC00000u
#define MAC_VIA_BASE         0xE80000u
#define MAC_DEBUG_BASE       0xF00000u

/* Mac Plus screen: 512x342, 1 bit/pixel. The main framebuffer sits a
 * fixed distance below the top of RAM. */
#define MAC_SCREEN_W   512
#define MAC_SCREEN_H   342
#define MAC_FB_BYTES   ((MAC_SCREEN_W / 8) * MAC_SCREEN_H)   /* 21888 */
#define MAC_FB_OFFSET  0x5900u
#define MAC_SND_OFFSET 0x0300u   /* sound buffer below the framebuffer */

/* Debug-port register offsets (relative to MAC_DEBUG_BASE). */
#define MAC_DBG_SERIAL  0x00u
#define MAC_DBG_EXIT    0x02u
#define MAC_DBG_CYCLES  0x04u

/* --- 6522 VIA ----------------------------------------------------------
 * The Mac's VIA owns the ROM overlay, sound, the RTC serial lines, the
 * keyboard (via the shift register), the mouse button and the vertical-
 * blank interrupt. Its IRQ output drives 68000 IPL 1. */
typedef struct via6522 {
    u8  ora, orb;          /* output registers A / B            */
    u8  ira, irb;          /* input latches                     */
    u8  ddra, ddrb;        /* data-direction registers          */
    u8  acr, pcr;          /* auxiliary / peripheral control    */
    u8  ifr, ier;          /* interrupt flag / enable           */
    u8  sr;                /* shift register (keyboard)         */
    u8  icb2;              /* CB2 line — keyboard data          */
    u16 t1c, t1l;          /* timer 1 counter / latch           */
    u16 t2c;               /* timer 2 counter                   */
    u8  t2l_lo;            /* timer 2 low latch                 */
    bool t1_irq_armed;     /* T1 will raise IRQ on next timeout */
    bool t2_irq_armed;     /* T2 is one-shot: armed by T2C-H write,
                            * cleared when the IRQ fires            */
    u64 last_cycle;        /* cycle stamp of the last timer step */
    u32 via_tick_rem;      /* CPU-cycle remainder for the /10 divider */
    u64 next_vbl;          /* next CA1 / vertical-blank edge     */
} via6522;

/* VIA IFR/IER bits. */
#define VIA_IRQ_CA2  0x01u   /* one-second interrupt (from RTC) */
#define VIA_IRQ_CA1  0x02u   /* vertical blank                  */
#define VIA_IRQ_SR   0x04u   /* shift register (keyboard)       */
#define VIA_IRQ_CB2  0x08u
#define VIA_IRQ_CB1  0x10u
#define VIA_IRQ_T2   0x20u
#define VIA_IRQ_T1   0x40u
#define VIA_IRQ_ANY  0x80u

/* --- Apple RTC ---------------------------------------------------------
 * A small serial clock/PRAM chip bit-banged over VIA PB0/PB1/PB2. */
typedef struct mac_rtc {
    u8  state;             /* serial state machine phase        */
    u8  shift;             /* bit shifter                       */
    u8  bit_count;
    u8  cmd;               /* latched command byte              */
    bool data_out;         /* current value driven onto PB0     */
    bool last_clk;         /* PB1 edge detection                */
    u32 seconds;           /* clock — seconds since 1904        */
    u8  pram[256];         /* parameter RAM                     */
    bool write_pending;
} mac_rtc;

/* --- IWM floppy controller --------------------------------------------- */
typedef struct mac_iwm {
    u8  lines;             /* ca0/ca1/ca2/lstrb/motor/drive bits */
    u8  q6, q7;            /* register-select latches            */
    u8  mode;              /* IWM mode register                  */
    bool motor_on;
    int  drive;            /* selected head/side (0/1)           */
    u8  sel;               /* SEL line (from VIA PA5)            */
    int  step_dir;         /* 0 = toward higher track, 1 = lower */
    int  cur_track;        /* physical head position (0..79)     */
    u8   tach;             /* tachometer toggle                  */

    /* Disk image — logical-sector order, 819200 bytes for an 800K disk. */
    u8  *disk;
    u32  disk_size;
    bool disk_present;
    bool disk_wprot;

    /* Synthesised GCR nibble stream for the track under the head. */
    u8   trackbuf[16384];
    u32  track_len;
    int  track, side;      /* which (track,side) trackbuf holds  */
    u32  nib_pos;
    u64  dbg_data_reads;   /* diagnostic: GCR nibbles handed out */
} mac_iwm;

/* --- SCC (Zilog 8530) ------------------------------------------------- */
/* Two channels (A = modem port, B = printer port). Each has its own
 * 16-entry WR / RR file plus a tiny Tx/Rx FIFO. The Mac touches the
 * control port through the address-low pattern: bit 1 = data/control,
 * bit 2 = channel (A=1, B=0). */
typedef struct mac_scc_chan {
    u8   wr[16];           /* write-register file                       */
    u8   rr[16];           /* read-register file (status, vector, etc.) */
    u8   rx_byte;          /* last byte buffered from external source   */
    u8   tx_byte;          /* last byte the guest wrote to data reg     */
    bool rx_avail;         /* set by external source; cleared on data read */
    bool tx_pending;       /* set by guest Tx; cleared by sink callback  */
} mac_scc_chan;

typedef struct mac_scc {
    u8   wr_ptr;           /* control-port register pointer (channel-shared) */
    bool irq_on;           /* guest enabled SCC interrupts (WR9 MIE) —
                            * the Mac Plus mouse rides the SCC IRQ, so
                            * this gates host mouse injection */
    mac_scc_chan ch[2];    /* [0] = B (printer), [1] = A (modem) — index
                            * matches the address-bit-2 wiring */

    /* Tx callback fired whenever the guest writes a byte to a data port.
     * `channel` is 0 (B / printer) or 1 (A / modem). Set by the host;
     * NULL means bytes are dropped. */
    void (*tx_sink)(void *ctx, int channel, u8 byte);
    void *tx_ctx;
} mac_scc;

struct m68k_cpu;
typedef void (*mac_serial_fn)(void *ctx, u8 byte);

typedef struct mac_mem {
    u8  *ram;
    u32  ram_size;
    u8  *rom;
    u32  rom_size;
    /* size-1 when the size is a power of two (always, for real Mac RAM/ROM), so
     * address mirroring is a 1-cycle AND instead of a ~30-cycle runtime modulo
     * on every memory access. 0 = not power-of-two, fall back to %. */
    u32  ram_mask;
    u32  rom_mask;
    u64  mmio_ops;     /* diagnostic: count of MMIO (non-RAM/ROM) accesses */
    bool overlay;

    via6522  via;
    mac_rtc  rtc;
    mac_iwm  iwm;
    mac_scc  scc;
    mac_scsi scsi;

    u32  fb_base;          /* active framebuffer address          */
    u8   mouse_btn;        /* 0 = pressed, 1 = up                 */
    i16  mouse_dx, mouse_dy;

    mac_serial_fn serial_sink;
    void         *serial_ctx;

    /* Sound: called from mac_mem_tick at each VBL with 370 bytes of
     * 8-bit unsigned PCM mono samples pulled from the active sound
     * buffer at the top of RAM (see mac_snd.c). The sink is responsible
     * for whatever happens next (queue to SDL audio, drop, file, ...).
     * NULL = silent. */
    void (*snd_sink)(void *ctx, const u8 *samples, u32 n);
    void *snd_ctx;
    mac_snd_filter snd_filter;

    struct m68k_cpu *cpu;

    u64  reads, writes;
} mac_mem;

extern void (*mac_mmio_log)(mac_mem *m, u32 addr, u32 val, int is_write, int size);

/* Write-watch hook — the JIT dispatcher registers this to catch the guest
 * overwriting code it has already compiled (self-modifying code / segment
 * reuse). Called for every write to RAM. */
extern void (*mac_write_watch)(void *ctx, u32 addr);
extern void  *mac_write_watch_ctx;

void mac_mem_init(mac_mem *m, u32 ram_size);
void mac_mem_free(mac_mem *m);
bool mac_load_rom(mac_mem *m, const u8 *rom, u32 len);
void mac_load_ram_image(mac_mem *m, u32 addr, const u8 *img, u32 len);

/* Insert a logical-sector-order floppy image into the IWM (drive 0). */
bool mac_insert_disk(mac_mem *m, const u8 *img, u32 len, bool write_protected);

/* As above, but into a specific drive (0-based). The Mac Plus has two. */
bool mac_insert_disk_drive(mac_mem *m, int drive, const u8 *img, u32 len,
                           bool write_protected);

/* SCC external I/O. Inject one received byte on `channel` (0=B/printer,
 * 1=A/modem) — sets the Rx-char-available bit and raises an IRQ if
 * Rx interrupts are enabled. Tx bytes from the guest are delivered to
 * `mac_scc.tx_sink` (set directly on the struct). */
void mac_scc_rx(mac_mem *m, int channel, u8 byte);

u8  mac_read8 (mac_mem *m, u32 addr);
u16 mac_read16(mac_mem *m, u32 addr);
u32 mac_read32(mac_mem *m, u32 addr);
void mac_write8 (mac_mem *m, u32 addr, u8  v);
void mac_write16(mac_mem *m, u32 addr, u16 v);
void mac_write32(mac_mem *m, u32 addr, u32 v);

/* Inline fast-path accessors for the interpreter's hot loop: resolve the
 * RAM/ROM host pointer inline (the overwhelmingly common case) and read/write
 * directly, falling back to the out-of-line MMIO-aware functions only when the
 * address isn't plain memory. This removes a function call + the region decode
 * from every opcode fetch, operand fetch and data access. */
static inline u8 *mac_ptr_fast(const mac_mem *m, u32 addr) {
    if (!m->overlay) {
        if (addr < MAC_ROM_BASE)
            return m->ram_size ? m->ram + (m->ram_mask ? (addr & m->ram_mask)
                                                        : (addr % m->ram_size)) : 0;
        if (addr < MAC_ROM_BASE + 0x100000u)
            return m->rom ? m->rom + (m->rom_mask ? ((addr - MAC_ROM_BASE) & m->rom_mask)
                                                   : ((addr - MAC_ROM_BASE) % m->rom_size)) : 0;
        return 0;   /* MMIO */
    }
    if (addr < 0x100000u)
        return m->rom ? m->rom + (m->rom_mask ? (addr & m->rom_mask)
                                              : (addr % m->rom_size)) : 0;
    if (addr >= MAC_OVL_RAM_BASE && addr < MAC_OVL_RAM_BASE + 0x100000u && m->ram_size)
        return m->ram + (m->ram_mask ? ((addr - MAC_OVL_RAM_BASE) & m->ram_mask)
                                     : ((addr - MAC_OVL_RAM_BASE) % m->ram_size));
    if (addr >= MAC_ROM_BASE && addr < MAC_ROM_BASE + 0x100000u)
        return m->rom ? m->rom + (m->rom_mask ? ((addr - MAC_ROM_BASE) & m->rom_mask)
                                              : ((addr - MAC_ROM_BASE) % m->rom_size)) : 0;
    return 0;
}
static inline u16 mac_read16_fast(mac_mem *m, u32 addr) {
    addr &= 0xFFFFFFu;
    const u8 *p = mac_ptr_fast(m, addr);
    if (p) return (u16)(((u16)p[0] << 8) | p[1]);
    return mac_read16(m, addr);
}
static inline u32 mac_read32_fast(mac_mem *m, u32 addr) {
    addr &= 0xFFFFFFu;
    const u8 *p = mac_ptr_fast(m, addr);
    if (p) return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
    return mac_read32(m, addr);
}
static inline u8 mac_read8_fast(mac_mem *m, u32 addr) {
    addr &= 0xFFFFFFu;
    const u8 *p = mac_ptr_fast(m, addr);
    if (p) return *p;
    return mac_read8(m, addr);
}
/* Writable-RAM host pointer, or NULL (ROM / MMIO / write-watcher active). The
 * write-watcher (JIT SMC tracking) must see every RAM write, so when it's
 * registered (JIT builds) we force the slow path; on the interpreter build it's
 * NULL and the branch predicts away. */
extern void (*mac_write_watch)(void *ctx, u32 addr);
static inline u8 *mac_ram_wptr(const mac_mem *m, u32 addr) {
    if (mac_write_watch) return 0;
    if (!m->overlay)
        return (addr < MAC_ROM_BASE && m->ram_size)
                   ? m->ram + (m->ram_mask ? (addr & m->ram_mask) : (addr % m->ram_size)) : 0;
    if (addr >= MAC_OVL_RAM_BASE && addr < MAC_OVL_RAM_BASE + 0x100000u && m->ram_size)
        return m->ram + (m->ram_mask ? ((addr - MAC_OVL_RAM_BASE) & m->ram_mask)
                                     : ((addr - MAC_OVL_RAM_BASE) % m->ram_size));
    return 0;
}
static inline void mac_write16_fast(mac_mem *m, u32 addr, u16 v) {
    addr &= 0xFFFFFFu;
    u8 *p = mac_ram_wptr(m, addr);
    if (p) { p[0] = (u8)(v >> 8); p[1] = (u8)v; return; }
    mac_write16(m, addr, v);
}
static inline void mac_write32_fast(mac_mem *m, u32 addr, u32 v) {
    addr &= 0xFFFFFFu;
    u8 *p = mac_ram_wptr(m, addr);
    if (p) { p[0]=(u8)(v>>24); p[1]=(u8)(v>>16); p[2]=(u8)(v>>8); p[3]=(u8)v; return; }
    mac_write32(m, addr, v);
}
static inline void mac_write8_fast(mac_mem *m, u32 addr, u8 v) {
    addr &= 0xFFFFFFu;
    u8 *p = mac_ram_wptr(m, addr);
    if (p) { *p = v; return; }
    mac_write8(m, addr, v);
}

void mac_mem_tick(mac_mem *m, u64 cycles);

/* Recompute the VIA IRQ summary and cpu->pending_irq. Public so the
 * keyboard code (core/mac_input.c) can raise the shift-register IRQ. */
void mac_via_recalc_irq(mac_mem *m);

/* True once the guest has enabled SCC interrupts — the cue that the OS
 * is up and ready for mouse input. */
static inline bool mac_mouse_enabled(const mac_mem *m) {
    return m->scc.irq_on;
}

#endif
