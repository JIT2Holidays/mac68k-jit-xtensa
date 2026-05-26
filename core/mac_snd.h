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

/* Mac Plus native sample rate (370 samples × 60 Hz VBL). */
#define MAC_SND_SAMPLE_RATE      22255

/* Audio band-pass: two 2nd-order Butterworth biquads in series.
 *   - LPF anti-alias: default cutoff at Nyquist (11127 Hz)
 *     env MAC68K_AUDIO_CUTOFF=<hz>
 *   - HPF DC-block:   default cutoff 20 Hz (subsonic — kills the
 *     DC offset that pegs host VU meters at idle but produces no
 *     audible sound, without touching audible bass)
 *     env MAC68K_AUDIO_HPF=<hz>
 *   MAC68K_AUDIO_FILTER=off bypasses both. Both stages are
 *   direct-form-I biquads in their own state. */
typedef struct mac_snd_biquad {
    double b0, b1, b2, a1, a2;
    double x1, x2, y1, y2;
} mac_snd_biquad;

typedef struct mac_snd_filter {
    bool           enabled;
    mac_snd_biquad lp;          /* low-pass anti-alias       */
    mac_snd_biquad hp;          /* high-pass DC blocker      */
} mac_snd_filter;

void mac_snd_filter_init(struct mac_snd_filter *f,
                         double fc_lp_hz, double fc_hp_hz, double fs_hz);

/* Pull `count` samples (up to MAC_SND_SAMPLES_PER_VBL) for the current
 * VBL from the buffer selected by VIA PA3, run them through the
 * filter, and write 8-bit unsigned samples to `out`. Safe to call even
 * before the OS has set up the buffer (reads return whatever is in
 * RAM, usually zeros = silence). */
void mac_snd_extract_vbl(struct mac_mem *m, u8 *out);

#endif
