#ifndef MAC_MEM_H
#define MAC_MEM_H

#include "m68k_types.h"
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
    u16 t1c, t1l;          /* timer 1 counter / latch           */
    u16 t2c;               /* timer 2 counter                   */
    u8  t2l_lo;            /* timer 2 low latch                 */
    bool t1_irq_armed;     /* T1 will raise IRQ on next timeout */
    u64 last_cycle;        /* cycle stamp of the last timer step */
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

/* --- SCC (Zilog 8530) — reduced model ---------------------------------- */
typedef struct mac_scc {
    u8  reg_ptr[2];        /* selected register, per channel    */
    u8  wr[2][16];
} mac_scc;

struct m68k_cpu;
typedef void (*mac_serial_fn)(void *ctx, u8 byte);

typedef struct mac_mem {
    u8  *ram;
    u32  ram_size;
    u8  *rom;
    u32  rom_size;
    bool overlay;

    via6522  via;
    mac_rtc  rtc;
    mac_iwm  iwm;
    mac_scc  scc;

    u32  fb_base;          /* active framebuffer address          */
    u8   mouse_btn;        /* 0 = pressed, 1 = up                 */
    i16  mouse_dx, mouse_dy;

    mac_serial_fn serial_sink;
    void         *serial_ctx;

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

u8  mac_read8 (mac_mem *m, u32 addr);
u16 mac_read16(mac_mem *m, u32 addr);
u32 mac_read32(mac_mem *m, u32 addr);
void mac_write8 (mac_mem *m, u32 addr, u8  v);
void mac_write16(mac_mem *m, u32 addr, u16 v);
void mac_write32(mac_mem *m, u32 addr, u32 v);

void mac_mem_tick(mac_mem *m, u64 cycles);

#endif
