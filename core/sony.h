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

/* Re-point the driver's mac_mem pointer without resetting state. Used
 * by the diff-jit-trace lockstep to keep sony's writes routed to the
 * engine currently running (sony.c's S.vm is global; without this
 * re-pointing, sony_service / extension-trap writes from one engine's
 * run loop would land in the other engine's memory). */
void sony_set_vm(struct mac_mem *m);

/* Patch the ROM: install the replacement .Sony driver, the extension
 * trap, and the disk icon, then repair the ROM checksum. Call once after
 * the ROM image is loaded. Returns false if the ROM is too small. */
bool sony_patch_rom(struct mac_mem *m);

/* Provide a logical-sector floppy image to the emulated drive. */
bool sony_insert_disk(struct mac_mem *m, const u8 *img, u32 len, bool wprot);

/* As above, but into a specific drive (0-based; the Mac Plus has two). */
bool sony_insert_disk_drive(struct mac_mem *m, int drive, const u8 *img,
                            u32 len, bool wprot);

/* --- streaming disk backend (on-demand sectors) -----------------------
 * Serves a drive from a backing store too large to hold in RAM (e.g. a
 * multi-hundred-MB image on an SD card) instead of an in-RAM image copy.
 * The callbacks transfer a byte range to/from the disk; the .Sony driver
 * only ever issues 512-byte-aligned, block-granular I/O, so `offset` and
 * `len` are always multiples of 512. `read` is required; a NULL `write`
 * marks the drive read-only. Returning false reports a drive I/O error
 * to the guest. This is purely additive: drives attached the legacy way
 * (sony_insert_disk*) leave the backend NULL and behave exactly as before. */
typedef struct sony_disk_backend {
    bool (*read )(void *ctx, u32 offset, u8 *dst, u32 len);
    bool (*write)(void *ctx, u32 offset, const u8 *src, u32 len);
    void *ctx;
    u32   size;   /* logical disk size in bytes (multiple of 512) */
    bool  wprot;
} sony_disk_backend;

/* Attach a streaming backend to drive `drive` (0-based). Marks the drive
 * inserted and unmounted, exactly like sony_insert_disk_drive, but keeps
 * no in-RAM copy of the image. Returns false on a bad drive index or a
 * backend without a read callback. */
bool sony_attach_backend(struct mac_mem *m, int drive,
                         const sony_disk_backend *be);

/* Handle a word write to the extension trap window (offset 0..0x1F). */
void sony_extn_write(u32 offset, u32 data);

/* Called at ~60 Hz; counts down the insert delay. */
void sony_tick(void);

/* Called from the CPU run loop between instructions. If a disk insert is
 * pending, injects the mount pseudo-exception and returns true. */
bool sony_service(struct m68k_cpu *cpu);

#endif
