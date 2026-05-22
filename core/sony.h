#ifndef SONY_H
#define SONY_H

#include "m68k_types.h"

/* Macintosh .Sony floppy driver emulation, ported from mini vMac
 * (third_party/minivmac, SONYEMDV.c / ROMEMDEV.c).
 *
 * Rather than emulate the IWM/GCR hardware, this replaces the .Sony disk
 * driver inside the ROM with a small 68k stub (mini vMac's `sony_driver`)
 * that traps into the emulator. The emulator then serves whole logical
 * sectors straight out of the disk image — exactly mini vMac's approach.
 *
 * Integration with mac_mem.c:
 *   - sony_patch_rom()   patches the ROM image after it is loaded
 *   - sony_extn_write()  handles writes to the extension trap address
 *   - sony_tick()        called at the VBL rate; arms a pending insert
 *   - sony_service()     called from the run loop; injects the disk-
 *                        inserted pseudo-exception when one is pending */

struct mac_mem;
struct m68k_cpu;

/* The extension trap address (mini vMac's kExtn_Block_Base for the Plus).
 * A 32-byte MMIO window; the patched driver pokes an ExtnDat pointer here. */
#define SONY_EXTN_BASE  0x00F40000u

void sony_init(struct mac_mem *m);

/* Patch the ROM: install the replacement .Sony driver, the extension
 * trap, and the disk icon, then repair the ROM checksum. Call once after
 * the ROM image is loaded. Returns false if the ROM is too small. */
bool sony_patch_rom(struct mac_mem *m);

/* Provide a logical-sector floppy image to the emulated drive. */
bool sony_insert_disk(struct mac_mem *m, const u8 *img, u32 len, bool wprot);

/* As above, but into a specific drive (0-based; the Mac Plus has two). */
bool sony_insert_disk_drive(struct mac_mem *m, int drive, const u8 *img,
                            u32 len, bool wprot);

/* Handle a word write to the extension trap window (offset 0..0x1F). */
void sony_extn_write(u32 offset, u32 data);

/* Called at ~60 Hz; counts down the insert delay. */
void sony_tick(void);

/* Called from the CPU run loop between instructions. If a disk insert is
 * pending, injects the mount pseudo-exception and returns true. */
bool sony_service(struct m68k_cpu *cpu);

#endif
