/* Macintosh .Sony floppy driver emulation.
 *
 * Ported from mini vMac (third_party/minivmac): the 68k `sony_driver`
 * stub and disk icon come verbatim from ROMEMDEV.c, and the extension
 * dispatch (Open/Prime/Control/Status/Mount) is adapted from SONYEMDV.c.
 *
 * The ROM's real .Sony driver is overwritten with the stub; the stub
 * builds an "ExtnDat" parameter block on the stack and pokes its address
 * to the extension trap address, which the emulator intercepts here. */

#include "sony.h"
#include "mac_mem.h"
#include "m68k_cpu.h"
#include <string.h>
#include <stdlib.h>

/* --- the mini vMac replacement .Sony driver (ROMEMDEV.c, verbatim) ----- */

static const u8 sony_driver[] = {
    0x4F,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xF4,0x00,0x18,
    0x00,0x24,0x00,0x4A,0x00,0x8A,0x05,0x2E,0x53,0x6F,0x6E,0x79,
    0x48,0xE7,0x00,0xC0,0x55,0x4F,0x3F,0x3C,0x00,0x01,0x60,0x30,
    0x48,0xE7,0x00,0xC0,0x55,0x4F,0x3F,0x3C,0x00,0x02,0x41,0xFA,
    0x01,0xA4,0x2F,0x18,0x20,0x50,0x20,0x8F,0x5C,0x4F,0x30,0x1F,
    0x4C,0xDF,0x03,0x00,0x0C,0x68,0x00,0x01,0x00,0x1A,0x66,0x1E,
    0x4E,0x75,0x48,0xE7,0x00,0xC0,0x55,0x4F,0x3F,0x3C,0x00,0x03,
    0x41,0xFA,0x01,0x7E,0x2F,0x18,0x20,0x50,0x20,0x8F,0x5C,0x4F,
    0x30,0x1F,0x4C,0xDF,0x03,0x00,0x32,0x28,0x00,0x06,0x08,0x01,
    0x00,0x09,0x67,0x0C,0x4A,0x40,0x6F,0x02,0x42,0x40,0x31,0x40,
    0x00,0x10,0x4E,0x75,0x4A,0x40,0x6F,0x04,0x42,0x40,0x4E,0x75,
    0x2F,0x38,0x08,0xFC,0x4E,0x75,0x48,0xE7,0x00,0xC0,0x55,0x4F,
    0x3F,0x3C,0x00,0x04,0x41,0xFA,0x01,0x3E,0x2F,0x18,0x20,0x50,
    0x20,0x8F,0x5C,0x4F,0x30,0x1F,0x4C,0xDF,0x03,0x00,0x4E,0x75,
    0x48,0xE7,0xE0,0xC0,0x20,0x2F,0x00,0x14,0x59,0x4F,0x2F,0x00,
    0x55,0x4F,0x3F,0x3C,0x00,0x08,0x41,0xFA,0x01,0x18,0x2F,0x18,
    0x20,0x50,0x20,0x8F,0x5C,0x4F,0x32,0x1F,0x58,0x4F,0x20,0x1F,
    0x4A,0x41,0x66,0x06,0x30,0x7C,0x00,0x07,0xA0,0x2F,0x4C,0xDF,
    0x03,0x07,0x58,0x4F,0x4E,0x73,0x21,0x40,0x00,0x06,0x43,0xF8,
    0x03,0x08,0x4E,0xF9,0x00,0x40,0x0B,0x20,0x31,0x78,0x0E,0x19,
    0x00,0x0A,0x4E,0x75,0x48,0xE7,0x1F,0x18,0x48,0xE7,0x00,0xC0,
    0x5D,0x4F,0x3F,0x3C,0x00,0x05,0x41,0xFA,0x00,0xD0,0x2F,0x18,
    0x20,0x50,0x20,0x8F,0x5C,0x4F,0x30,0x1F,0x2E,0x1F,0x0C,0x40,
    0xFF,0xCF,0x66,0x06,0x42,0x40,0x60,0x00,0x00,0xA8,0x4A,0x40,
    0x66,0x00,0x00,0xA2,0x20,0x07,0xA7,0x1E,0x28,0x48,0x20,0x0C,
    0x67,0x00,0x00,0xA0,0x9E,0xFC,0x00,0x10,0x2F,0x0C,0x2F,0x07,
    0x55,0x4F,0x3F,0x3C,0x00,0x06,0x41,0xFA,0x00,0x94,0x2F,0x18,
    0x20,0x50,0x20,0x8F,0x5C,0x4F,0x30,0x1F,0x50,0x4F,0x2E,0x1F,
    0x76,0x00,0x36,0x1F,0x38,0x1F,0x3C,0x1F,0x3A,0x1F,0x26,0x5F,
    0x4A,0x40,0x66,0x64,0x41,0xEC,0x00,0x30,0x43,0xFA,0xFF,0x86,
    0x31,0x7C,0x00,0x01,0x00,0x04,0x21,0x49,0x00,0x06,0x31,0x78,
    0x0E,0x19,0x00,0x0A,0xA0,0x33,0x20,0x0B,0x67,0x0E,0x41,0xFA,
    0xFF,0x72,0x27,0x48,0x00,0x06,0x20,0x4B,0xA0,0x58,0x60,0x1A,
    0x41,0xFA,0xFF,0x50,0x30,0x3C,0xA0,0x4E,0xA0,0x47,0x60,0x0E,
    0x20,0x47,0x30,0x06,0x48,0x40,0x30,0x05,0xA0,0x4E,0xDE,0x83,
    0x52,0x46,0x51,0xCC,0xFF,0xF0,0x48,0x7A,0xFE,0xFC,0x55,0x4F,
    0x3F,0x3C,0x00,0x07,0x41,0xFA,0x00,0x1E,0x2F,0x18,0x20,0x50,
    0x20,0x8F,0x5C,0x4F,0x30,0x1F,0x58,0x4F,0x4C,0xDF,0x03,0x00,
    0x4C,0xDF,0x18,0xF8,0x4E,0x75,0x30,0x3C,0xFF,0xFF,0x60,0xF0,
};

static const u8 my_disk_icon[] = {
    0x7F,0xFF,0xFF,0xF0,0x81,0x00,0x01,0x08,0x81,0x00,0x71,0x04,
    0x81,0x00,0x89,0x02,0x81,0x00,0x89,0x01,0x81,0x00,0x89,0x01,
    0x81,0x00,0x89,0x01,0x81,0x00,0x89,0x01,0x81,0x00,0x89,0x01,
    0x81,0x00,0x71,0x01,0x81,0x00,0x01,0x01,0x80,0xFF,0xFE,0x01,
    0x80,0x00,0x00,0x01,0x80,0x00,0x00,0x01,0x80,0x00,0x00,0x01,
    0x80,0x00,0x00,0x01,0x83,0xFF,0xFF,0xC1,0x84,0x00,0x00,0x21,
    0x84,0x00,0x00,0x21,0x84,0x00,0x00,0x21,0x84,0x00,0x00,0x21,
    0x84,0x00,0x00,0x21,0x84,0x06,0x30,0x21,0x84,0x06,0x60,0x21,
    0x84,0x06,0xC0,0x21,0x84,0x07,0x80,0x21,0x84,0x07,0x00,0x21,
    0x84,0x06,0x00,0x21,0x84,0x00,0x00,0x21,0x84,0x00,0x00,0x21,
    0x84,0x00,0x00,0x21,0x7F,0xFF,0xFF,0xFE,0x3F,0xFF,0xFF,0xF0,
    0x7F,0xFF,0xFF,0xF0,0xFF,0xFF,0xFF,0xFC,0xFF,0xFF,0xFF,0xFC,
    0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
    0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
    0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
    0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
    0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
    0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
    0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
    0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
    0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0x7F,0xFF,0xFF,0xFC,
    0x3F,0xFF,0xFF,0xFC,0x00,0x00,
};

/* --- constants (from SONYEMDV.c / GLOBGLUE.h) -------------------------- */

#define SONY_DRIVER_BASE  0x17D30u   /* .Sony in the Mac Plus ROM */
#define kcom_callcheck    0x5B17u
#define kExtnSony         2
#define kExtnDisk         1
#define kcom_checkval     0x841339E2u

#define ExtnDat_extension 2
#define ExtnDat_commnd    4
#define ExtnDat_result    6
#define ExtnDat_params    8
#define ExtnDat_version   8

#define kCmndVersion      0
#define kCmndSonyPrime    1
#define kCmndSonyControl  2
#define kCmndSonyStatus   3
#define kCmndSonyClose    4
#define kCmndSonyOpenA    5
#define kCmndSonyOpenB    6
#define kCmndSonyOpenC    7
#define kCmndSonyMount    8

/* Mac OS error codes. */
#define mnvm_noErr        0x0000
#define mnvm_miscErr      0xFFFF
#define mnvm_controlErr   0xFFEF
#define mnvm_statusErr    0xFFEE
#define mnvm_closErr      0xFFE8
#define mnvm_eofErr       0xFFD9
#define mnvm_wPrErr       0xFFD4
#define mnvm_opWrErr      0xFFCF
#define mnvm_paramErr     0xFFCE
#define mnvm_nsDrvErr     0xFFC8
#define mnvm_offLinErr    0xFFBF

/* Parameter-block field offsets. */
#define kioTrap        6
#define kioResult     16
#define kioVRefNum    22
#define kcsCode       26
#define kcsParam      28
#define kioBuffer     32
#define kioReqCount   36
#define kioActCount   40
#define kdCtlPosition 16

/* Drive-variable field offsets. */
#define kWriteProt    2
#define kDiskInPlace  3
#define kInstalled    4
#define kSides        5
#define kQLink        6
#define kQType       10
#define kQDriveNo    12
#define kQRefNum     14
#define kQFSID       16
#define kQDrvSz      18
#define kQDrvSz2     20
#define kTwoSideFmt  18
#define kNewIntf     19
#define kDriveErrs   20

/* Control / Status csCodes. */
#define kVerifyDisk    5
#define kFormatDisk    6
#define kEjectDisk     7
#define kSetTagBuffer  8
#define kTrackCacheControl 9
#define kDriveIcon    21
#define kDriveInfo    23
#define kDriveStatus   8

/* SonyVars layout (Mac Plus). */
#define SonyVarsPtr           0x0134
#define FirstDriveVarsOffset  0x004A
#define EachDriveVarsSize     0x0042
#define MinSonVarsSize        0x00000310

#define NUM_DRIVES  2

/* --- module state ------------------------------------------------------ */

static struct {
    mac_mem *vm;
    u8  *image[NUM_DRIVES];
    u32  size[NUM_DRIVES];
    bool inserted[NUM_DRIVES];
    bool mounted[NUM_DRIVES];
    bool wprot[NUM_DRIVES];

    u32  mount_callback;     /* 68k routine, set by Sony OpenC */
    u32  param_addr_hi;      /* extension-trap hi-word latch   */
    u32  disk_icon_addr;     /* ROM address of the disk icon   */
    int  insert_delay;       /* VBL countdown before mounting  */
    bool insert_pending;     /* a mount pseudo-exception is due */
} S;

/* --- VM access helpers ------------------------------------------------- */

static u8  vget8 (u32 a)        { return mac_read8 (S.vm, a); }
static u16 vget16(u32 a)        { return mac_read16(S.vm, a); }
static u32 vget32(u32 a)        { return mac_read32(S.vm, a); }
static void vput8 (u32 a, u8 v) { mac_write8 (S.vm, a, v); }
static void vput16(u32 a, u16 v){ mac_write16(S.vm, a, v); }
static void vput32(u32 a, u32 v){ mac_write32(S.vm, a, v); }

static void vmove(u32 dst, u32 src, u32 n) {
    for (u32 i = 0; i < n; i++) vput8(dst + i, vget8(src + i));
}

/* --- init / disk / ROM patch ------------------------------------------- */

void sony_init(mac_mem *m) {
    memset(&S, 0, sizeof(S));
    S.vm = m;
}

bool sony_insert_disk_drive(mac_mem *m, int d, const u8 *img, u32 len,
                            bool wprot) {
    (void)m;
    if (d < 0 || d >= NUM_DRIVES) return false;
    free(S.image[d]);
    S.image[d] = (u8 *)malloc(len);
    if (!S.image[d]) return false;
    memcpy(S.image[d], img, len);
    S.size[d] = len;
    S.inserted[d] = true;
    S.mounted[d] = false;
    S.wprot[d] = wprot;
    return true;
}

bool sony_insert_disk(mac_mem *m, const u8 *img, u32 len, bool wprot) {
    return sony_insert_disk_drive(m, 0, img, len, wprot);
}

bool sony_patch_rom(mac_mem *m) {
    if (!m->rom || m->rom_size < SONY_DRIVER_BASE + sizeof(sony_driver) + 300)
        return false;
    u8 *rom = m->rom;
    u8 *pto = rom + SONY_DRIVER_BASE;

    memcpy(pto, sony_driver, sizeof(sony_driver));
    pto += sizeof(sony_driver);

    /* extension trap: [callcheck][kExtnSony][trap address] */
    pto[0] = (u8)(kcom_callcheck >> 8); pto[1] = (u8)kcom_callcheck; pto += 2;
    pto[0] = 0; pto[1] = kExtnSony; pto += 2;
    pto[0] = (u8)(SONY_EXTN_BASE >> 24); pto[1] = (u8)(SONY_EXTN_BASE >> 16);
    pto[2] = (u8)(SONY_EXTN_BASE >> 8);  pto[3] = (u8)SONY_EXTN_BASE;  pto += 4;

    S.disk_icon_addr = (u32)(pto - rom) + MAC_ROM_BASE;
    memcpy(pto, my_disk_icon, sizeof(my_disk_icon));

    /* The boot ROM verifies its own 32-bit word checksum (stored in the
     * first long); patching it invalidates that, so recompute and store
     * the new checksum — the sum of every word past the first two. */
    u32 sum = 0;
    for (u32 i = 4; i + 1 < m->rom_size; i += 2)
        sum += ((u32)rom[i] << 8) | rom[i + 1];
    rom[0] = (u8)(sum >> 24); rom[1] = (u8)(sum >> 16);
    rom[2] = (u8)(sum >> 8);  rom[3] = (u8)sum;
    return true;
}

/* --- disk transfer ----------------------------------------------------- */

static u16 check_drive(int d) {
    if (d < 0 || d >= NUM_DRIVES || !S.inserted[d]) return mnvm_nsDrvErr;
    return mnvm_noErr;
}

/* Copy between the disk image and emulated memory. */
static u16 drive_transfer(bool is_write, u32 buffer, int d,
                          u32 start, u32 count, u32 *act) {
    if (act) *act = 0;
    u16 r = check_drive(d);
    if (r != mnvm_noErr) return r;
    if (is_write && S.wprot[d]) return mnvm_wPrErr;
    if (start > S.size[d]) return mnvm_eofErr;
    bool eof = false;
    u32 L = S.size[d] - start;
    if (L >= count) L = count; else eof = true;
    for (u32 i = 0; i < L; i++) {
        if (is_write) S.image[d][start + i] = vget8(buffer + i);
        else          vput8(buffer + i, S.image[d][start + i]);
    }
    if (act) *act = L;
    return eof ? mnvm_eofErr : mnvm_noErr;
}

/* --- drive-variable access -------------------------------------------- */

static u32 drive_vars(int d) {
    u32 sv = vget32(SonyVarsPtr);
    if (d < 0 || d >= NUM_DRIVES) return 0;
    return sv + FirstDriveVarsOffset + EachDriveVarsSize * (u32)d;
}

/* --- the Sony driver commands (ported from SONYEMDV.c) ----------------- */

static u16 sony_prime(u32 p) {
    u32 pb  = vget32(p + ExtnDat_params + 0);
    u32 dce = vget32(p + ExtnDat_params + 4);
    int d   = (int)vget16(pb + kioVRefNum) - 1;
    u16 trap = vget16(pb + kioTrap);
    u32 dvl = drive_vars(d);
    u16 result;
    u32 act = 0;

    if (dvl == 0) { result = mnvm_nsDrvErr; goto done; }
    if (0xA002 != (trap & 0xF0FE)) { result = mnvm_controlErr; goto done; }
    {
        bool is_write = (trap & 1) != 0;
        u8 dip = vget8(dvl + kDiskInPlace);
        if (dip != 0x02) {
            if (dip == 0x01) vput8(dvl + kDiskInPlace, 0x02);
            else { result = mnvm_offLinErr; goto done; }
        }
        u32 start = vget32(dce + kdCtlPosition);
        u32 count = vget32(pb + kioReqCount);
        if ((start & 0x1FF) || (count & 0x1FF)) {
            result = mnvm_paramErr;
        } else if (is_write && vget8(dvl + kWriteProt) != 0) {
            result = mnvm_wPrErr;
        } else {
            u32 buffer = vget32(pb + kioBuffer);
            result = drive_transfer(is_write, buffer, d, start, count, &act);
            vput32(dce + kdCtlPosition, start + act);
        }
    }
done:
    vput16(pb + kioResult, result);
    vput32(pb + kioActCount, act);
    if (result != mnvm_noErr) vput16(0x0142 /* DskErr */, result);
    return result;
}

static u16 sony_control(u32 p) {
    u32 pb = vget32(p + ExtnDat_params + 0);
    u16 op = vget16(pb + kcsCode);
    u16 result;

    if (op == 1 /*kKillIO*/) {
        result = mnvm_miscErr;
    } else if (op == kSetTagBuffer || op == kTrackCacheControl) {
        result = mnvm_noErr;          /* tags unsupported; pretend ok */
    } else {
        int d = (int)vget16(pb + kioVRefNum) - 1;
        u32 dvl = drive_vars(d);
        if (dvl == 0) {
            result = mnvm_nsDrvErr;
        } else if (vget8(dvl + kDiskInPlace) == 0) {
            result = mnvm_offLinErr;
        } else switch (op) {
            case kVerifyDisk:
            case kFormatDisk:
                result = mnvm_noErr;
                break;
            case kEjectDisk:
                vput8(dvl + kWriteProt, 0);
                vput8(dvl + kDiskInPlace, 0);
                vput16(dvl + kQRefNum, 0xFFFB);
                S.mounted[d] = false;
                result = mnvm_noErr;
                break;
            case kDriveIcon:
            case 20 /*kGetIconID*/:
            case 22 /*kMediaIcon*/:
                if (vget16(dvl + kQType) != 0) {
                    vput32(pb + kcsParam, S.disk_icon_addr);
                    result = mnvm_noErr;
                } else {
                    result = mnvm_controlErr;
                }
                break;
            case kDriveInfo: {
                u32 v = (vget16(dvl + kQType) != 0) ? 0x00000001u
                                                    : 0x00000003u; /* 800K */
                if (d != 0) v += 0x00000900u;
                vput32(pb + kcsParam, v);
                result = mnvm_noErr;
                break;
            }
            default:
                result = mnvm_controlErr;
                break;
        }
    }
    if (result != mnvm_noErr) vput16(0x0142, result);
    return result;
}

static u16 sony_status(u32 p) {
    u32 pb = vget32(p + ExtnDat_params + 0);
    u16 op = vget16(pb + kcsCode);
    u16 result;

    if (op == kDriveStatus) {
        int d = (int)vget16(pb + kioVRefNum) - 1;
        u32 src = drive_vars(d);
        if (src == 0) {
            result = mnvm_nsDrvErr;
        } else {
            if (S.insert_delay > 4) S.insert_delay = 4;
            vmove(pb + kcsParam, src, 22);
            result = mnvm_noErr;
        }
    } else {
        result = mnvm_statusErr;
    }
    if (result != mnvm_noErr) vput16(0x0142, result);
    return result;
}

static u16 sony_open_a(u32 p) {
    if (S.mount_callback != 0) return mnvm_opWrErr;
    u32 L = FirstDriveVarsOffset + EachDriveVarsSize * NUM_DRIVES;
    if (L < MinSonVarsSize) L = MinSonVarsSize;
    vput32(p + ExtnDat_params + 0, L);
    return mnvm_noErr;
}

static u16 sony_open_b(u32 p) {
    u32 sony_vars = vget32(p + ExtnDat_params + 4);
    u32 dce       = vget32(p + ExtnDat_params + 28);

    vput32(sony_vars + 16, kcom_checkval);
    vput32(sony_vars + 20, SONY_EXTN_BASE);
    vput16(sony_vars + 24, NUM_DRIVES);
    vput16(sony_vars + 26, kExtnDisk);
    vput32(SonyVarsPtr, sony_vars);

    for (int i = 0; i < NUM_DRIVES; i++) {
        u32 dvl = sony_vars + FirstDriveVarsOffset + EachDriveVarsSize * (u32)i;
        vput8 (dvl + kDiskInPlace, 0x00);
        vput8 (dvl + kInstalled, 0x01);
        vput8 (dvl + kSides, 0xFF);          /* double sided */
        vput16(dvl + kQDriveNo, (u16)(i + 1));
        vput16(dvl + kQRefNum, 0xFFFB);      /* .Sony */
    }

    /* route the hard-disk unit-table slot at the .Sony driver too */
    {
        u32 utable = vget32(0x011C);
        vput32(utable + 4 * 1, vget32(utable + 4 * 4));
    }
    vput8(dce + 7, 1);                       /* driver version */

    vput32(p + ExtnDat_params + 8,
           sony_vars + FirstDriveVarsOffset + kQLink);
    vput16(p + ExtnDat_params + 12, EachDriveVarsSize);
    vput16(p + ExtnDat_params + 14, NUM_DRIVES);
    vput16(p + ExtnDat_params + 16, 1);
    vput16(p + ExtnDat_params + 18, 0xFFFB);
    vput32(p + ExtnDat_params + 20, sony_vars + 28 /* NullTask */);
    return mnvm_noErr;
}

static u16 sony_open_c(u32 p) {
    S.mount_callback = vget32(p + ExtnDat_params + 0);
    return mnvm_noErr;
}

static u16 sony_mount(u32 p) {
    u32 data = vget32(p + ExtnDat_params + 0);
    int d = (int)(data & 0xFFFF);
    u32 dvl = drive_vars(d);
    if (dvl == 0) return mnvm_nsDrvErr;
    if (vget8(dvl + kDiskInPlace) != 0x00) return mnvm_miscErr;

    u32 blocks = S.size[d] >> 9;
    if (blocks == 800 || blocks == 1600) {
        vput8 (dvl + kTwoSideFmt, blocks == 800 ? 0x00 : 0xFF);
        vput8 (dvl + kNewIntf, 0xFF);
        vput16(dvl + kQType, 0x00);
        vput16(dvl + kDriveErrs, 0x0000);
    } else {
        vput16(dvl + kQRefNum, 0xFFFE);
        vput16(dvl + kQType, 0x01);
        vput16(dvl + kQDrvSz, (u16)blocks);
        vput16(dvl + kQDrvSz2, (u16)(blocks >> 16));
    }
    vput8(dvl + kWriteProt, (u8)(data >> 16));
    vput8(dvl + kDiskInPlace, 0x01);
    vput32(p + ExtnDat_params + 4, (u32)(d + 1));   /* disk-inserted event */
    S.mounted[d] = true;
    return mnvm_noErr;
}

/* --- extension dispatch ------------------------------------------------ */

static void extn_sony_access(u32 p) {
    u16 result;
    switch (vget16(p + ExtnDat_commnd)) {
        case kCmndVersion:    vput16(p + ExtnDat_version, 0);
                              result = mnvm_noErr; break;
        case kCmndSonyPrime:   result = sony_prime(p);   break;
        case kCmndSonyControl: result = sony_control(p); break;
        case kCmndSonyStatus:  result = sony_status(p);  break;
        case kCmndSonyClose:   result = mnvm_closErr;    break;
        case kCmndSonyOpenA:   result = sony_open_a(p);  break;
        case kCmndSonyOpenB:   result = sony_open_b(p);  break;
        case kCmndSonyOpenC:   result = sony_open_c(p);  break;
        case kCmndSonyMount:   result = sony_mount(p);   break;
        default:               result = mnvm_controlErr; break;
    }
    vput16(p + ExtnDat_result, result);
}

/* The patched driver pokes the 32-bit ExtnDat-block address here, split
 * across two word writes (hi at offset 0, lo at offset 2). */
void sony_extn_write(u32 offset, u32 data) {
    u32 reg = (offset >> 1) & 0x0F;
    if (reg == 0) {
        S.param_addr_hi = data & 0xFFFF;
    } else if (reg == 1) {
        u32 p = (S.param_addr_hi << 16) | (data & 0xFFFF);
        S.param_addr_hi = 0xFFFFFFFFu;
        if (vget16(p /* ExtnDat_checkval */) == kcom_callcheck) {
            vput16(p, 0);
            if (vget16(p + ExtnDat_extension) == kExtnSony)
                extn_sony_access(p);
            else
                vput16(p + ExtnDat_result, mnvm_controlErr);
        }
    }
}

/* --- disk-inserted pseudo-exception ----------------------------------- */

void sony_tick(void) {
    if (S.insert_delay > 0) S.insert_delay--;
}

bool sony_service(m68k_cpu *cpu) {
    if (S.mount_callback == 0 || S.insert_delay > 0) return false;
    /* find an inserted-but-unmounted drive */
    int d = -1;
    for (int i = 0; i < NUM_DRIVES; i++)
        if (S.inserted[i] && !S.mounted[i]) { d = i; break; }
    if (d < 0) return false;

    /* Mark it scheduled so we don't fire every instruction; the mount
     * pseudo-exception runs the driver callback which calls sony_mount. */
    S.mounted[d] = true;

    u32 data = (u32)d;
    if (S.wprot[d]) data |= 0x00FF0000u;

    /* ExceptionTo(mount_callback): push PC + SR, enter supervisor,
     * then push `data` for the callback. */
    bool was_super = m68k_is_super(cpu);
    u16 saved_sr = cpu->sr;
    cpu->sr |= SR_S;
    cpu->sr &= (u16)~SR_T;
    m68k_sync_sp(cpu, was_super);
    cpu->a[7] -= 4; mac_write32(S.vm, cpu->a[7], cpu->pc);
    cpu->a[7] -= 2; mac_write16(S.vm, cpu->a[7], saved_sr);
    cpu->pc = S.mount_callback;
    cpu->a[7] -= 4; mac_write32(S.vm, cpu->a[7], data);
    return true;
}
