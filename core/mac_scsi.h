#ifndef MAC_SCSI_H
#define MAC_SCSI_H

#include "m68k_types.h"

struct mac_mem;

/* Mac Plus NCR 5380 SCSI controller + one HD target.
 *
 * The 5380's register file is exposed at MAC_SCSI_BASE with each
 * register at a 16-byte stride (Mac Plus pin wiring). Eight registers:
 *
 *   0  Current SCSI data       (R: bus state, W: output data)
 *   1  Initiator command       (control signal latches)
 *   2  Mode                    (DMA enable, monitor BSY, ...)
 *   3  Target command          (C/D, I/O, MSG signals)
 *   4  R: Current SCSI status  W: Select-enable
 *   5  R: Bus & status
 *   6  R: Input data           W: Start DMA send
 *   7  R: Reset parity / interrupt
 *
 * The Mac Plus also exposes a pseudo-DMA window in the same region;
 * we ignore the pseudo-DMA addresses for now — the boot ROM and the
 * .Sony-installed SCSI driver fall back to non-DMA mode when DMA is
 * not advertised (we don't set the DRQ/EOP bits).
 *
 * Single target wired at SCSI ID 0 — a 512-byte-sector HD backed by a
 * disk image file. ID 7 is the initiator (the Mac). */

#define MAC_SCSI_NUM_TARGETS 7

typedef struct mac_scsi_target {
    bool present;                /* true if the target responds         */
    bool write_protect;
    u8  *image;                  /* host-side disk-image bytes          */
    u32  bytes;                  /* image size, must be a multiple of 512 */
    u32  blocks;                 /* derived: bytes / 512                */
    /* INQUIRY response (36 bytes). Filled in by mac_scsi_attach. */
    u8   inquiry[36];
} mac_scsi_target;

typedef struct mac_scsi {
    /* Register file as the guest sees it; some bits are recomputed live
     * on read (current bus phase, BSY/REQ/ACK signals). */
    u8 odr;                      /* register 0 W: Output Data Register   */
    u8 icr;                      /* register 1: Initiator Command         */
    u8 mr;                       /* register 2: Mode                      */
    u8 tcr;                      /* register 3: Target Command            */
    u8 ser;                      /* register 4 W: Select Enable           */

    /* Bus state. */
    u8   phase;                  /* SCSI_PHASE_*                          */
    int  target_id;              /* selected target (0..6) or -1          */

    /* Command / data buffers. CDB is up to 12 bytes; payload is up to
     * 64 KB (Mac OS does its 512-byte block reads via short CDBs and
     * usually keeps individual transfers ≤ 32 KB). */
    u8   cdb[12];
    u8   cdb_len;
    u8   cdb_pos;
    u8  *data;                   /* points into the target image, or a
                                  * static reply buffer for INQUIRY etc.  */
    u32  data_len;
    u32  data_pos;
    bool data_to_initiator;      /* true = target → initiator (READ)      */
    u8   status;                 /* SCSI status byte (0 = GOOD)           */
    u8   message;                /* COMMAND COMPLETE = 0                  */
    /* Scratch buffer for short replies (INQUIRY, READ CAPACITY, ...). */
    u8   reply_buf[64];

    /* Targets. */
    mac_scsi_target targets[MAC_SCSI_NUM_TARGETS];
} mac_scsi;

/* SCSI bus phases. */
enum {
    SCSI_PHASE_BUS_FREE = 0,
    SCSI_PHASE_ARBITRATION,
    SCSI_PHASE_SELECTION,
    SCSI_PHASE_COMMAND,
    SCSI_PHASE_DATA_IN,
    SCSI_PHASE_DATA_OUT,
    SCSI_PHASE_STATUS,
    SCSI_PHASE_MSG_IN,
    SCSI_PHASE_MSG_OUT,
};

void mac_scsi_init(struct mac_scsi *s);
void mac_scsi_free(struct mac_scsi *s);

/* Attach an HD image as a target at `id` (0..6, default 0). Takes
 * ownership of `img` (will free on mac_scsi_free). Returns false if
 * `id` is already attached or out of range. */
bool mac_scsi_attach_disk(struct mac_scsi *s, int id, u8 *img, u32 len,
                          bool write_protect);

/* Read/write a register at offset `reg_off` (0..7 << 4). */
u8   mac_scsi_reg_read(struct mac_scsi *s, u32 reg);
void mac_scsi_reg_write(struct mac_scsi *s, u32 reg, u8 val);

#endif
