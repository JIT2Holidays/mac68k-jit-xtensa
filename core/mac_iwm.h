#ifndef MAC_IWM_H
#define MAC_IWM_H

#include "m68k_types.h"
#include "mac_mem.h"

/* IWM ("Integrated Woz Machine") floppy controller + the Apple 3.5"
 * 800 KB GCR disk encoding.
 *
 * The IWM is reached at 0xC00000-0xDFFFFF; its 16 register addresses
 * each toggle one control line and read/write one of four registers
 * selected by the Q6/Q7 latches. When the drive motor is on, reading the
 * data register returns the next nibble of the GCR bitstream for the
 * track under the head — synthesised on the fly from a logical-sector
 * disk image. */

void iwm_init(mac_iwm *iwm);
bool iwm_insert(mac_iwm *iwm, const u8 *img, u32 len, bool write_protected);

u8   iwm_read (mac_iwm *iwm, u32 addr);
void iwm_write(mac_iwm *iwm, u32 addr, u8 val);

/* --- Apple RTC (declared here, implemented in mac_mem.c) --------------- */
bool rtc_data_out(mac_rtc *r);
void rtc_via_write(mac_rtc *r, u8 pb);

#endif
