/* Remote control of the headless Mac over the USB-Serial-JTAG console.
 *
 * Lets a host drive the (display-less) board: dump the Mac framebuffer as
 * base64 to view it, and inject mouse / keyboard events — including firmware-
 * timed single/double clicks (the Mac samples the mouse per VBL and times
 * double-clicks, so the press/release spacing must be done in guest cycles).
 *
 * Line protocol (commands are newline-terminated ASCII; coords in Mac
 * screen pixels 0..511 / 0..341):
 *   d                screen dump  -> "FB:512,342,21888\n<base64>\n:FBEND\n"
 *   m <x> <y>        move mouse (button up)
 *   b <x> <y> <0|1>  raw mouse: move + set button
 *   c <x> <y>        single click at (x,y)
 *   C <x> <y>        double click at (x,y)
 *   k <code> <0|1>   raw key up/down (Mac raw keycode)
 *   K <code>         key tap (down then up)
 * Each command is acked with "OK\n".
 */

#ifndef UART_CTL_H
#define UART_CTL_H

#include "m68k_cpu.h"
#include "mac_mem.h"

/* Install the USB-Serial-JTAG driver and route stdio through it. */
void uart_ctl_init(void);

/* Poll for host commands and apply any now-due timed events. Call once per
 * run-loop chunk with the current cpu/mem. */
void uart_ctl_poll(m68k_cpu *cpu, mac_mem *m);

/* True while a timed click/key sequence is in flight — the run loop should
 * then advance in small cycle chunks so the press/release land spaced. */
bool uart_ctl_busy(void);

#endif /* UART_CTL_H */
