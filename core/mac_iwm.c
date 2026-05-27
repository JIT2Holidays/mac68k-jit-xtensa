/* IWM floppy controller + Apple 3.5" 800 KB GCR disk encoding, and the
 * Apple RTC serial chip.
 *
 * The IWM exposes 16 register addresses; each access toggles one control
 * line (ca0/ca1/ca2/lstrb/motor/drive/Q6/Q7) and reads or writes one of
 * four registers. With the motor on, the data register streams the GCR
 * nibbles of the track under the head, synthesised from a logical-sector
 * image. The drive answers status queries (disk-in-place, track-0, …) on
 * the IWM "sense" bit. */

#include "mac_iwm.h"
#include <stdlib.h>
#include <string.h>

/* --- Apple RTC --------------------------------------------------------- */

bool rtc_data_out(mac_rtc *r) {
    /* PB0 tristates whenever the RTC isn't actively driving it: either CE
     * deasserted, or we're between command/data phases. The real Mac bus
     * pulls high. We model this by returning HIGH unless the chip is in
     * state 2 (shifting OUT a read response, the only time it actively
     * drives the line). The SE/30 boot ROM polls PB0 during idle for some
     * status check and expects HIGH — without this it loops forever. */
    if (r->state != 2) return true;
    return r->data_out;
}

/* Drive the RTC from a VIA port-B write: PB0=data, PB1=clock, PB2=/enable. */
void rtc_via_write(mac_rtc *r, u8 pb) {
    bool enb = (pb & 0x04) == 0;       /* /rtcEnb active low */
    bool clk = (pb & 0x02) != 0;
    bool data = (pb & 0x01) != 0;

    r->ce_high = !enb;
    if (!enb) {                        /* deselected — reset the transfer */
        r->state = 0;
        r->bit_count = 0;
        r->last_clk = clk;
        return;
    }
    bool rising = clk && !r->last_clk;
    r->last_clk = clk;
    if (!rising) return;

    if (r->state == 0) {               /* shifting in the command byte */
        r->shift = (u8)((r->shift << 1) | (data ? 1 : 0));
        if (++r->bit_count == 8) {
            r->cmd = r->shift;
            r->bit_count = 0;
            r->state = (r->cmd & 0x80) ? 2 : 1;   /* 2 = read, 1 = write */
            if (r->state == 2) {
                /* Preload the value to shift out. Clock bytes 0..3 live
                 * in bits [6:5] of the command; PRAM otherwise (zeroed). */
                u8 v = 0;
                switch (r->cmd & 0x7F) {
                    case 0x01: v = (u8)(r->seconds      ); break;
                    case 0x05: v = (u8)(r->seconds >>  8); break;
                    case 0x09: v = (u8)(r->seconds >> 16); break;
                    case 0x0D: v = (u8)(r->seconds >> 24); break;
                    default:   v = r->pram[r->cmd & 0x1F]; break;
                }
                r->shift = v;
            }
        }
    } else if (r->state == 1) {        /* shifting in a write data byte */
        r->shift = (u8)((r->shift << 1) | (data ? 1 : 0));
        if (++r->bit_count == 8) {
            r->pram[r->cmd & 0x1F] = r->shift;
            r->bit_count = 0;
            r->state = 0;
        }
    } else {                           /* shifting out a read data byte */
        r->data_out = (r->shift & 0x80) != 0;
        r->shift = (u8)(r->shift << 1);
        if (++r->bit_count == 8) {
            r->bit_count = 0;
            r->state = 0;
        }
    }
}

/* --- GCR 6-and-2 encoding ---------------------------------------------- */

/* 6-bit value -> disk nibble. */
static const u8 gcr6[64] = {
    0x96,0x97,0x9A,0x9B,0x9D,0x9E,0x9F,0xA6,0xA7,0xAB,0xAC,0xAD,0xAE,0xAF,0xB2,0xB3,
    0xB4,0xB5,0xB6,0xB7,0xB9,0xBA,0xBB,0xBC,0xBD,0xBE,0xBF,0xCB,0xCD,0xCE,0xCF,0xD3,
    0xD6,0xD7,0xD9,0xDA,0xDB,0xDC,0xDD,0xDE,0xDF,0xE5,0xE6,0xE7,0xE9,0xEA,0xEB,0xEC,
    0xED,0xEE,0xEF,0xF2,0xF3,0xF4,0xF5,0xF6,0xF7,0xF9,0xFA,0xFB,0xFC,0xFD,0xFE,0xFF
};

/* Sectors per track by speed zone (800 KB, double-sided). */
static int sectors_in_track(int track) {
    if (track < 16) return 12;
    if (track < 32) return 11;
    if (track < 48) return 10;
    if (track < 64) return 9;
    return 8;
}

/* Logical block number of (track, side, sector). Tracks are stored
 * side 0 then side 1 is interleaved per the Sony geometry: the Mac
 * numbers blocks track-by-track, both sides. */
static int logical_block(int track, int side, int sector) {
    int blk = 0;
    for (int t = 0; t < track; t++) blk += sectors_in_track(t) * 2;
    blk += side * sectors_in_track(track);
    return blk + sector;
}

/* Encode the 12 tag + 512 data = 524-byte payload of one sector into the
 * GCR data-field nibble stream. Returns the number of nibbles written
 * (699 data symbols + 4 checksum symbols = 703).
 *
 * This is the canonical Apple 3.5" "6-and-2" nibblize: the 524 bytes are
 * consumed three at a time through three running checksums (c1/c2/c3),
 * each group producing a "twos" symbol carrying the spilled high bit-
 * pairs followed by three (or two) data symbols. */
static int gcr_encode_data(const u8 *src, u8 *out) {
    u32 a = 0, b = 0, c = 0, carry = 0;
    int i = 0, o = 0;
    while (i < 524) {
        /* rotate checksum C left; the bit shifted out is the carry into
         * byte 1's accumulator add. */
        carry = (c >> 7) & 1;
        c = ((c << 1) | carry) & 0xFF;

        /* byte 1 -> accumulator A, output byte ^ C */
        u8 v1 = src[i++];
        a += v1 + carry; carry = (a > 0xFF); a &= 0xFF;
        u8 w1 = (u8)(v1 ^ c);

        /* byte 2 -> accumulator B, output byte ^ A */
        u8 v2 = src[i++];
        b += v2 + carry; carry = (b > 0xFF); b &= 0xFF;
        u8 w2 = (u8)(v2 ^ a);

        /* byte 3 -> accumulator C, output byte ^ B */
        u8 w3 = 0;
        bool has3 = (i < 524);
        if (has3) {
            u8 v3 = src[i++];
            c += v3 + carry; c &= 0xFF;
            w3 = (u8)(v3 ^ b);
        }
        u8 w4 = (u8)((((w1 >> 6) & 3) << 4) | (((w2 >> 6) & 3) << 2)
                     | ((w3 >> 6) & 3));
        out[o++] = gcr6[w4 & 0x3F];
        out[o++] = gcr6[w1 & 0x3F];
        out[o++] = gcr6[w2 & 0x3F];
        if (has3) out[o++] = gcr6[w3 & 0x3F];
    }
    /* C is rotated once after every group, including the last, before the
     * checksum is emitted. */
    carry = (c >> 7) & 1;
    c = ((c << 1) | carry) & 0xFF;
    /* trailing 6&2-encoded 24-bit checksum (A,B,C). */
    u8 w4 = (u8)(((a & 0xC0) >> 2) | ((b & 0xC0) >> 4) | ((c & 0xC0) >> 6));
    out[o++] = gcr6[w4 & 0x3F];
    out[o++] = gcr6[a & 0x3F];
    out[o++] = gcr6[b & 0x3F];
    out[o++] = gcr6[c & 0x3F];
    return o;
}

/* Build the full GCR nibble stream for one physical track (one side). */
static void build_track(mac_iwm *iwm, int track, int side) {
    u8 *t = iwm->trackbuf;
    int o = 0;
    int nsec = sectors_in_track(track);
    int fmt = 0x22;                        /* 800 KB, double sided */
    for (int s = 0; s < nsec; s++) {
        /* self-sync gap */
        for (int g = 0; g < 36; g++) t[o++] = 0xFF;
        /* address field */
        t[o++] = 0xD5; t[o++] = 0xAA; t[o++] = 0x96;
        u8 tk = (u8)(track & 0x3F);
        u8 sd = (u8)((side ? 0x20 : 0) | ((track >> 6) & 1));
        u8 ck = (u8)(tk ^ s ^ sd ^ fmt);
        t[o++] = gcr6[tk & 0x3F];
        t[o++] = gcr6[s  & 0x3F];
        t[o++] = gcr6[sd & 0x3F];
        t[o++] = gcr6[fmt & 0x3F];
        t[o++] = gcr6[ck & 0x3F];
        t[o++] = 0xDE; t[o++] = 0xAA; t[o++] = 0xFF;
        /* data field */
        for (int g = 0; g < 6; g++) t[o++] = 0xFF;
        t[o++] = 0xD5; t[o++] = 0xAA; t[o++] = 0xAD;
        t[o++] = gcr6[s & 0x3F];
        u8 payload[524];
        memset(payload, 0, 12);            /* tag bytes */
        int blk = logical_block(track, side, s);
        if (iwm->disk && (u32)(blk + 1) * 512u <= iwm->disk_size)
            memcpy(payload + 12, iwm->disk + blk * 512, 512);
        else
            memset(payload + 12, 0, 512);
        o += gcr_encode_data(payload, t + o);
        t[o++] = 0xDE; t[o++] = 0xAA; t[o++] = 0xFF;
    }
    iwm->track_len = o;
    iwm->track = track;
    iwm->side = side;
    iwm->nib_pos = 0;
}

/* --- IWM control ------------------------------------------------------- */

void iwm_init(mac_iwm *iwm) {
    memset(iwm, 0, sizeof(*iwm));
    iwm->cur_track = 0;
}

bool iwm_insert(mac_iwm *iwm, const u8 *img, u32 len, bool wprot) {
    free(iwm->disk);
    iwm->disk = (u8 *)malloc(len);
    if (!iwm->disk) return false;
    memcpy(iwm->disk, img, len);
    iwm->disk_size = len;
    iwm->disk_present = true;
    iwm->disk_wprot = wprot;
    iwm->track = -1;                       /* force a rebuild on first read */
    return true;
}

/* The drive answers a status query selected by ca2,ca1,ca0 and the SEL
 * line. Returns the SENSE bit (0/1). */
static int iwm_sense(mac_iwm *iwm) {
    int reg = (iwm->lines & 1)            /* ca0 */
            | ((iwm->lines >> 1) & 1) << 1 /* ca1 */
            | ((iwm->lines >> 2) & 1) << 2 /* ca2 */
            | (iwm->sel ? 8 : 0);
    switch (reg) {
        case 0x0: return iwm->step_dir;             /* DIRTN  */
        case 0x1: return iwm->disk_present ? 0 : 1; /* CSTIN: 0 = disk in */
        case 0x2: return 1;                         /* STEP: 1 = step done */
        case 0x3: return iwm->disk_wprot ? 0 : 1;   /* WRTPRT: 0 = protected */
        case 0x4: return iwm->motor_on ? 0 : 1;     /* MOTORON: 0 = on */
        case 0x5: return (iwm->cur_track == 0) ? 0 : 1; /* TKO: 0 = at trk 0 */
        case 0x6: return 0;                         /* SWITCHED */
        case 0x7: return iwm->tach ^= 1;            /* TACH pulses */
        case 0x8: return 0;                         /* RDDATA0 */
        case 0x9: return 0;                         /* RDDATA1 */
        case 0xB: return 1;                         /* SIDES: 1 = double */
        case 0xC: return 0;
        case 0xD: return iwm->disk_present ? 0 : 1; /* DRVIN / READY */
        case 0xF: return 1;
        default:  return 0;
    }
}

/* Apply a control-line strobe and run the side effects (stepping). */
static void iwm_set_line(mac_iwm *iwm, int line, int val) {
    u8 bit = (u8)(1u << line);
    u8 old = iwm->lines;
    if (val) iwm->lines |= bit; else iwm->lines &= (u8)~bit;

    switch (line) {
        case 4: iwm->motor_on = val; break;          /* ENABLE */
        case 5: iwm->drive = val; break;             /* DRIVE select */
        case 3:                                      /* LSTRB */
            /* A rising LSTRB latches a drive command from ca0/ca1; the
             * Mac uses it to issue STEP / motor / eject. We honour STEP. */
            if (val && !(old & (1u << 3))) {
                int cmd = (iwm->lines & 1) | ((iwm->lines >> 1) & 1) << 1
                        | ((iwm->lines >> 2) & 1) << 2;
                if (cmd == 1) {                      /* STEP pulse */
                    if (iwm->step_dir) {
                        if (iwm->cur_track > 0) iwm->cur_track--;
                    } else {
                        if (iwm->cur_track < 79) iwm->cur_track++;
                    }
                    iwm->track = -1;                 /* invalidate cache */
                } else if (cmd == 0) {
                    iwm->step_dir = (iwm->lines & 2) ? 1 : 0;
                }
            }
            break;
        default: break;
    }
}

u8 iwm_read(mac_iwm *iwm, u32 addr) {
    int n = (int)((addr >> 9) & 0xF);
    int line = n >> 1, val = n & 1;
    /* Every access toggles its control line; the read then returns the
     * register currently selected by Q6/Q7 — including accesses whose
     * line *is* Q6 or Q7. */
    if (line == 6)      iwm->q6 = (u8)val;
    else if (line == 7) iwm->q7 = (u8)val;
    else                iwm_set_line(iwm, line, val);

    if (iwm->q7 == 0 && iwm->q6 == 0) {
        /* DATA register: stream GCR nibbles while the motor runs. */
        if (!iwm->motor_on || !iwm->disk_present) return 0;
        if (iwm->track != iwm->cur_track || iwm->side != iwm->drive)
            build_track(iwm, iwm->cur_track, iwm->drive);
        if (iwm->track_len == 0) return 0;
        u8 b = iwm->trackbuf[iwm->nib_pos];
        iwm->nib_pos = (iwm->nib_pos + 1) % iwm->track_len;
        iwm->dbg_data_reads++;
        return b;
    }
    if (iwm->q7 == 0 && iwm->q6 == 1) {
        /* STATUS register: SENSE | motor | mode. */
        u8 st = iwm->mode & 0x1F;
        if (iwm->motor_on) st |= 0x20;
        if (iwm_sense(iwm)) st |= 0x80;
        return st;
    }
    if (iwm->q7 == 1 && iwm->q6 == 0) {
        /* HANDSHAKE register: data ready, no underrun. */
        return 0x80;
    }
    return 0;
}

void iwm_write(mac_iwm *iwm, u32 addr, u8 val) {
    int n = (int)((addr >> 9) & 0xF);
    int line = n >> 1, lv = n & 1;
    if (line == 6) { iwm->q6 = (u8)lv; }
    else if (line == 7) { iwm->q7 = (u8)lv; }
    else iwm_set_line(iwm, line, lv);
    if (iwm->q6 && iwm->q7 && iwm->motor_on == 0)
        iwm->mode = val;                  /* write the IWM mode register */
}
