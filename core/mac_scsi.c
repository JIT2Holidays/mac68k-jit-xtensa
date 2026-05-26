/* NCR 5380 SCSI controller + one HD target, just enough that the Mac
 * Plus boot ROM enumerates it via SCSIManager and Mac OS's SCSI driver
 * can read its boot blocks. Implements the standard bus phase loop
 * (arbitration → selection → command → data → status → message) and a
 * handful of mandatory commands (INQUIRY, TEST UNIT READY, REQUEST
 * SENSE, READ CAPACITY, MODE SENSE 6, READ 6/10, WRITE 6/10). */

#include "mac_scsi.h"
#include <stdlib.h>
#include <string.h>

/* NCR 5380 Initiator Command Register bits (write side). */
#define ICR_DBUS_ENABLE 0x01u
#define ICR_ATN         0x02u
#define ICR_SEL         0x04u
#define ICR_BSY         0x08u
#define ICR_ACK         0x10u
#define ICR_LA          0x20u
#define ICR_AIP         0x40u
#define ICR_RST         0x80u

/* Current SCSI Bus Status (register 4 read) bits. */
#define SR_DBP          0x01u
#define SR_SEL          0x02u
#define SR_IO           0x04u
#define SR_CD           0x08u
#define SR_MSG          0x10u
#define SR_REQ          0x20u
#define SR_BSY          0x40u
#define SR_RST          0x80u

/* Bus & Status Register (register 5 read) bits. */
#define BSR_ACK         0x01u
#define BSR_ATN         0x02u
#define BSR_BSY_ERR     0x04u
#define BSR_PHASE_MATCH 0x08u
#define BSR_IRQ         0x10u
#define BSR_PARITY_ERR  0x20u
#define BSR_DRQ         0x40u
#define BSR_END_DMA     0x80u

void mac_scsi_init(mac_scsi *s) {
    memset(s, 0, sizeof(*s));
    s->phase = SCSI_PHASE_BUS_FREE;
    s->target_id = -1;
}

void mac_scsi_free(mac_scsi *s) {
    for (int i = 0; i < MAC_SCSI_NUM_TARGETS; i++)
        free(s->targets[i].image);
    memset(s, 0, sizeof(*s));
}

bool mac_scsi_attach_disk(mac_scsi *s, int id, u8 *img, u32 len,
                          bool write_protect) {
    if (id < 0 || id >= MAC_SCSI_NUM_TARGETS) return false;
    if (s->targets[id].present) return false;
    if (len == 0 || (len & 0x1FFu) != 0) return false;     /* mult. of 512 */
    mac_scsi_target *t = &s->targets[id];
    t->present       = true;
    t->write_protect = write_protect;
    t->image         = img;
    t->bytes         = len;
    t->blocks        = len / 512u;
    /* Standard 36-byte INQUIRY response: peripheral type 0 (direct
     * access), Apple-ish vendor/product so the Mac SCSI Manager treats
     * it as a normal HD. ANSI SCSI-2. */
    static const u8 inq_template[36] = {
        0x00, 0x00, 0x02, 0x02, 31,   0x00, 0x00, 0x00,
        'M','A','C','6','8','K',' ',' ',                   /* vendor[8]   */
        'H','D',' ','I','m','a','g','e',' ',' ',' ',' ',' ',' ',' ',' ',
        '1','.','0',' '                                     /* revision[4] */
    };
    memcpy(t->inquiry, inq_template, sizeof(t->inquiry));
    return true;
}

/* --- Command processing ---------------------------------------------- */

static void scsi_set_data_in(mac_scsi *s, u8 *buf, u32 len) {
    s->phase             = SCSI_PHASE_DATA_IN;
    s->data              = buf;
    s->data_len          = len;
    s->data_pos          = 0;
    s->data_to_initiator = true;
    s->tcr               = SR_IO;                   /* I/O set = data-in   */
}

static void scsi_set_data_out(mac_scsi *s, u8 *buf, u32 len) {
    s->phase             = SCSI_PHASE_DATA_OUT;
    s->data              = buf;
    s->data_len          = len;
    s->data_pos          = 0;
    s->data_to_initiator = false;
    s->tcr               = 0;                       /* I/O clear = data-out */
}

static void scsi_finish(mac_scsi *s, u8 status) {
    s->status  = status;
    s->message = 0;
    s->phase   = SCSI_PHASE_STATUS;
    s->tcr     = SR_CD | SR_IO;                     /* status phase         */
}

static void scsi_execute(mac_scsi *s) {
    mac_scsi_target *t = (s->target_id >= 0 && s->target_id < MAC_SCSI_NUM_TARGETS)
                       ? &s->targets[s->target_id] : NULL;
    if (!t || !t->present) { scsi_finish(s, 0x02); return; }   /* CHECK */
    u8 op = s->cdb[0];
    switch (op) {
        case 0x00: /* TEST UNIT READY */
            scsi_finish(s, 0x00);
            return;
        case 0x03: /* REQUEST SENSE */
            memset(s->reply_buf, 0, 18);
            s->reply_buf[0] = 0x70;          /* current error, fixed format */
            s->reply_buf[7] = 10;            /* additional sense length     */
            scsi_set_data_in(s, s->reply_buf, 18);
            return;
        case 0x08: { /* READ(6) — LBA in 21 bits, len in 8 bits */
            u32 lba = ((u32)(s->cdb[1] & 0x1F) << 16)
                    | ((u32) s->cdb[2]         <<  8)
                    |  (u32) s->cdb[3];
            u32 n   = s->cdb[4] ? s->cdb[4] : 256;
            if ((u64)lba + n > t->blocks) { scsi_finish(s, 0x02); return; }
            scsi_set_data_in(s, t->image + lba * 512u, n * 512u);
            return;
        }
        case 0x0A: { /* WRITE(6) */
            u32 lba = ((u32)(s->cdb[1] & 0x1F) << 16)
                    | ((u32) s->cdb[2]         <<  8)
                    |  (u32) s->cdb[3];
            u32 n   = s->cdb[4] ? s->cdb[4] : 256;
            if ((u64)lba + n > t->blocks)    { scsi_finish(s, 0x02); return; }
            if (t->write_protect)            { scsi_finish(s, 0x02); return; }
            scsi_set_data_out(s, t->image + lba * 512u, n * 512u);
            return;
        }
        case 0x12: /* INQUIRY */
            scsi_set_data_in(s, t->inquiry, sizeof(t->inquiry));
            return;
        case 0x15: /* MODE SELECT(6) — accept and discard */
            scsi_finish(s, 0x00);
            return;
        case 0x1A: { /* MODE SENSE(6) — minimal 4-byte mode header */
            memset(s->reply_buf, 0, 4);
            s->reply_buf[0] = 3;             /* data length             */
            scsi_set_data_in(s, s->reply_buf, 4);
            return;
        }
        case 0x1B: /* START STOP UNIT */
        case 0x1E: /* PREVENT/ALLOW MEDIUM REMOVAL */
        case 0x35: /* SYNCHRONIZE CACHE */
            scsi_finish(s, 0x00);
            return;
        case 0x25: { /* READ CAPACITY(10) — returns (last LBA, block size) */
            u32 last = t->blocks - 1;
            s->reply_buf[0] = (u8)(last >> 24);
            s->reply_buf[1] = (u8)(last >> 16);
            s->reply_buf[2] = (u8)(last >>  8);
            s->reply_buf[3] = (u8) last;
            s->reply_buf[4] = 0;
            s->reply_buf[5] = 0;
            s->reply_buf[6] = 2;             /* 512 bytes per block */
            s->reply_buf[7] = 0;
            scsi_set_data_in(s, s->reply_buf, 8);
            return;
        }
        case 0x28: { /* READ(10) */
            u32 lba = ((u32)s->cdb[2] << 24) | ((u32)s->cdb[3] << 16)
                    | ((u32)s->cdb[4] <<  8) |  (u32)s->cdb[5];
            u32 n   = ((u32)s->cdb[7] <<  8) |  (u32)s->cdb[8];
            if (n == 0) { scsi_finish(s, 0x00); return; }
            if ((u64)lba + n > t->blocks) { scsi_finish(s, 0x02); return; }
            scsi_set_data_in(s, t->image + lba * 512u, n * 512u);
            return;
        }
        case 0x2A: { /* WRITE(10) */
            u32 lba = ((u32)s->cdb[2] << 24) | ((u32)s->cdb[3] << 16)
                    | ((u32)s->cdb[4] <<  8) |  (u32)s->cdb[5];
            u32 n   = ((u32)s->cdb[7] <<  8) |  (u32)s->cdb[8];
            if (n == 0) { scsi_finish(s, 0x00); return; }
            if ((u64)lba + n > t->blocks) { scsi_finish(s, 0x02); return; }
            if (t->write_protect)         { scsi_finish(s, 0x02); return; }
            scsi_set_data_out(s, t->image + lba * 512u, n * 512u);
            return;
        }
        default:
            scsi_finish(s, 0x02);            /* CHECK CONDITION */
            return;
    }
}

/* --- Bus phase advance ----------------------------------------------- */

/* Selection: the initiator writes the target ID OR'd with its own (7)
 * to the ODR while asserting SEL without BSY. We pick the first
 * non-7 bit set as the target. */
static void scsi_select(mac_scsi *s) {
    int sel = -1;
    for (int id = 0; id < MAC_SCSI_NUM_TARGETS; id++)
        if ((s->odr & (1u << id)) && s->targets[id].present) { sel = id; break; }
    if (sel < 0) {
        s->phase     = SCSI_PHASE_BUS_FREE;
        s->target_id = -1;
        return;
    }
    s->target_id = sel;
    s->phase     = SCSI_PHASE_COMMAND;
    s->tcr       = SR_CD;                /* command phase = C/D set, I/O clear */
    s->cdb_pos   = 0;
    s->cdb_len   = 0;
}

static u8 cdb_length_for(u8 op) {
    /* SCSI group: 0 = 6-byte, 1/2 = 10-byte, 5 = 12-byte. */
    int grp = (op >> 5) & 7;
    if (grp == 0) return 6;
    if (grp == 1 || grp == 2) return 10;
    if (grp == 5) return 12;
    return 6;
}

/* Called when the initiator strobes one CDB byte via the ODR + ACK. */
static void scsi_recv_cdb_byte(mac_scsi *s, u8 v) {
    if (s->cdb_pos < sizeof(s->cdb)) s->cdb[s->cdb_pos++] = v;
    if (s->cdb_pos == 1) s->cdb_len = cdb_length_for(v);
    if (s->cdb_pos >= s->cdb_len) scsi_execute(s);
}

/* --- Register file I/O ----------------------------------------------- */

/* Return the live `current SCSI bus status` (register 4 read) based on
 * the phase + target_id. */
static u8 scsi_bus_status(const mac_scsi *s) {
    u8 r = 0;
    switch (s->phase) {
        case SCSI_PHASE_COMMAND:
            r = SR_BSY | SR_CD | SR_REQ; break;
        case SCSI_PHASE_DATA_IN:
            r = SR_BSY | SR_IO | SR_REQ; break;
        case SCSI_PHASE_DATA_OUT:
            r = SR_BSY | SR_REQ;         break;
        case SCSI_PHASE_STATUS:
            r = SR_BSY | SR_CD | SR_IO | SR_REQ; break;
        case SCSI_PHASE_MSG_IN:
            r = SR_BSY | SR_CD | SR_IO | SR_MSG | SR_REQ; break;
        default: break;
    }
    return r;
}

u8 mac_scsi_reg_read(mac_scsi *s, u32 reg) {
    switch (reg & 7) {
        case 0: { /* Current SCSI data */
            if (s->phase == SCSI_PHASE_DATA_IN && s->data &&
                s->data_pos < s->data_len) {
                u8 v = s->data[s->data_pos++];
                if (s->data_pos >= s->data_len) scsi_finish(s, 0x00);
                return v;
            }
            if (s->phase == SCSI_PHASE_STATUS) {
                u8 st = s->status;
                s->phase = SCSI_PHASE_MSG_IN;
                return st;
            }
            if (s->phase == SCSI_PHASE_MSG_IN) {
                u8 ms = s->message;
                s->phase     = SCSI_PHASE_BUS_FREE;
                s->target_id = -1;
                s->tcr       = 0;
                return ms;
            }
            return 0;
        }
        case 1: return s->icr;
        case 2: return s->mr;
        case 3: return s->tcr;
        case 4: return scsi_bus_status(s);
        case 5: {
            u8 r = 0;
            if (s->phase != SCSI_PHASE_BUS_FREE) r |= BSR_PHASE_MATCH;
            return r;
        }
        case 6: return mac_scsi_reg_read(s, 0);    /* alias for current data */
        case 7: return 0;
    }
    return 0;
}

void mac_scsi_reg_write(mac_scsi *s, u32 reg, u8 val) {
    switch (reg & 7) {
        case 0: /* ODR: data to put on bus (selection ID or data-out byte) */
            s->odr = val;
            if (s->phase == SCSI_PHASE_DATA_OUT && s->data &&
                s->data_pos < s->data_len) {
                s->data[s->data_pos++] = val;
                if (s->data_pos >= s->data_len) scsi_finish(s, 0x00);
            } else if (s->phase == SCSI_PHASE_COMMAND) {
                scsi_recv_cdb_byte(s, val);
            }
            return;
        case 1: { /* ICR */
            u8 old = s->icr;
            s->icr = val;
            /* SEL asserted without BSY → selection phase. */
            if ((val & ICR_SEL) && !(old & ICR_SEL) &&
                !(val & ICR_BSY)) {
                s->phase = SCSI_PHASE_SELECTION;
                scsi_select(s);
            }
            /* RST asserted → bus reset. */
            if (val & ICR_RST) {
                s->phase     = SCSI_PHASE_BUS_FREE;
                s->target_id = -1;
                s->tcr       = 0;
            }
            return;
        }
        case 2: s->mr  = val; return;
        case 3: s->tcr = val; return;
        case 4: s->ser = val; return;
        case 5: case 6: case 7:
            /* Start DMA send/recv — we don't model the DMA edge; the
             * REAL transfer happens through register-0 reads/writes
             * above. Acknowledge by leaving the registers alone. */
            return;
    }
}
