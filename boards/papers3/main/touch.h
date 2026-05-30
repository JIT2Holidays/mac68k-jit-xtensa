/* GT911 capacitive touch -> Macintosh mouse, for the M5Stack PaperS3.
 *
 * The whole panel acts as a relative track-pad: dragging a finger moves the
 * Mac cursor by the scaled touch delta. A circular zone in the bottom-left of
 * the visible canvas (BOARD_BTN_* in board_papers3.h, drawn as a dotted
 * outline by eink.c) is the mouse button — touching it presses button 1.
 *
 * A task on core 1 polls the GT911 and maintains the cursor; the emulator
 * thread (core 0) pushes the latest state into the guest with
 * touch_apply_mouse(), so the Mac low-memory globals are only ever written
 * from the core running the CPU (no cross-core guest-RAM races). */

#ifndef TOUCH_H
#define TOUCH_H

struct mac_mem;

/* Initialize the GT911 (I2C) and launch the polling task on core 1. Safe to
 * call when the panel is absent — the mouse then simply never moves. */
void touch_start(void);

/* Apply the latest touch-pad cursor + button to the guest. Call from the
 * emulator thread (e.g. once per run-loop chunk). */
void touch_apply_mouse(struct mac_mem *m);

#endif /* TOUCH_H */
