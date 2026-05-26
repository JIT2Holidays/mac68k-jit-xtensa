#ifndef MAC_SND_H
#define MAC_SND_H

#include "m68k_types.h"

struct mac_mem;

/* Mac Plus sound: a 22.255 kHz, 8-bit PWM stream driven by the video
 * scanout DMA. Each VBL the hardware reads 370 sample bytes from one
 * of two sound buffers at the top of RAM:
 *
 *   main sound buffer:  RAM_top - 0x0300  (768 bytes; samples at even offsets)
 *   alt  sound buffer:  RAM_top - 0x5F00
 *
 * Selected by VIA PA3 (high = main).
 *
 * Sample byte layout: each pair of bytes holds [sample_lo, disk_speed].
 * The sample byte is the low 8 bits of a fixed-point unsigned PWM
 * level; we emit it directly as 8-bit unsigned mono PCM. */

#define MAC_SND_SAMPLES_PER_VBL  370u

/* Pull `count` samples (up to MAC_SND_SAMPLES_PER_VBL) for the current
 * VBL from the buffer selected by VIA PA3. Writes raw 8-bit unsigned
 * samples to `out`. Safe to call even before the OS has set up the
 * buffer (reads return whatever is in RAM, usually zeros = silence). */
void mac_snd_extract_vbl(struct mac_mem *m, u8 *out);

#endif
