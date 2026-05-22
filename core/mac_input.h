#ifndef MAC_INPUT_H
#define MAC_INPUT_H

#include "m68k_types.h"

/* Mouse and keyboard input for the emulated Macintosh.
 *
 * Mouse: the host position is written straight into the Mac low-memory
 * mouse globals (MTemp / RawMouse / CrsrNew), the way mini vMac does it.
 *
 * Keyboard: the Mac Plus keyboard is a serial device on the VIA shift
 * register. The state machine here is ported from mini vMac's
 * KBRDEMDV.c; mac_mem.c calls the via_* hooks when the guest drives the
 * VIA shift register, and key presses are queued with mac_key_event. */

struct mac_mem;

void mac_input_init(struct mac_mem *m);

/* Set the mouse position (Mac screen coordinates) and button state.
 * Safe to call every frame; injection is gated until the OS is up. */
void mac_set_mouse(struct mac_mem *m, int x, int y, bool button_down);

/* Queue a key transition. `mac_keycode` is a raw Macintosh key code
 * (0..127); see the table in the SDL GUI. */
void mac_key_event(struct mac_mem *m, int mac_keycode, bool down);

/* --- VIA shift-register hooks (called from mac_mem.c) ------------------ */
void mac_kbd_sr_written(struct mac_mem *m);          /* guest wrote VIA SR */
void mac_kbd_acr_written(struct mac_mem *m, u8 old_acr);
void mac_kbd_tick(struct mac_mem *m, u64 cycles);    /* timed kbd events */

#endif
