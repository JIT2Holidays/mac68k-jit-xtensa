#ifndef DEMO_ROM_H
#define DEMO_ROM_H

#include "m68k_types.h"

/* Builds the built-in demo program — a self-contained 68000 image used in
 * place of a real Macintosh boot ROM (which is copyrighted and not shipped
 * here). The program runs entirely from RAM: it sums 1..100, exercises the
 * shift / logic ops, fills a slice of the framebuffer, prints a result
 * line over the debug serial port, and signals completion through the
 * debug exit port.
 *
 * `out` must hold at least DEMO_ROM_MAX bytes. The image is laid out to be
 * loaded at address 0 (it contains its own reset vector). `fb_addr` is the
 * framebuffer address the program should draw into. Returns the image
 * length in bytes. */

#define DEMO_ROM_MAX  2048u

u32 demo_rom_build(u8 *out, u32 fb_addr);

#endif
