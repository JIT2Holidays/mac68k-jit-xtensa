/* Mac Plus sound: extract 8-bit PWM samples from the VIA-selected
 * sound buffer at the top of RAM, then run them through a 2nd-order
 * Butterworth IIR low-pass filter to smooth the raw PWM staircase.
 * See mac_snd.h for the layout. */

#include "mac_snd.h"
#include "mac_mem.h"
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* Robert Bristow-Johnson audio-EQ-cookbook biquad designs.
 * Pre-warps via the bilinear transform. fc must stay strictly below
 * fs/2 — at exactly Nyquist sin(ω₀)=0 collapses the LPF to unity gain
 * and the HPF to all-zero output. */
static void biquad_lpf_design(mac_snd_biquad *bq, double fc_hz, double fs_hz) {
    const double Q = 0.70710678118654752440;
    double nyq_safe = fs_hz * 0.499;
    if (fc_hz <= 0.0 || fc_hz > nyq_safe) fc_hz = nyq_safe;
    double w0    = 2.0 * M_PI * fc_hz / fs_hz;
    double cos_w = cos(w0);
    double alpha = sin(w0) / (2.0 * Q);
    double a0    = 1.0 + alpha;
    bq->b0 = (1.0 - cos_w) * 0.5 / a0;
    bq->b1 = (1.0 - cos_w)        / a0;
    bq->b2 = (1.0 - cos_w) * 0.5 / a0;
    bq->a1 = (-2.0 * cos_w)       / a0;
    bq->a2 = (1.0 - alpha)        / a0;
}

static void biquad_hpf_design(mac_snd_biquad *bq, double fc_hz, double fs_hz) {
    const double Q = 0.70710678118654752440;
    if (fc_hz <= 0.0) fc_hz = 1.0;
    double nyq_safe = fs_hz * 0.499;
    if (fc_hz > nyq_safe) fc_hz = nyq_safe;
    double w0    = 2.0 * M_PI * fc_hz / fs_hz;
    double cos_w = cos(w0);
    double alpha = sin(w0) / (2.0 * Q);
    double a0    = 1.0 + alpha;
    bq->b0 = (1.0 + cos_w) * 0.5 / a0;
    bq->b1 = -(1.0 + cos_w)       / a0;
    bq->b2 = (1.0 + cos_w) * 0.5 / a0;
    bq->a1 = (-2.0 * cos_w)       / a0;
    bq->a2 = (1.0 - alpha)        / a0;
}

static inline void biquad_reset_state(mac_snd_biquad *bq) {
    bq->x1 = bq->x2 = bq->y1 = bq->y2 = 0.0;
}

void mac_snd_filter_init(mac_snd_filter *f,
                         double fc_lp_hz, double fc_hp_hz, double fs_hz) {
    if (!f) return;
    f->enabled = true;
    biquad_lpf_design(&f->lp, fc_lp_hz, fs_hz);
    biquad_hpf_design(&f->hp, fc_hp_hz, fs_hz);
    biquad_reset_state(&f->lp);
    biquad_reset_state(&f->hp);
}

static inline double biquad_step(mac_snd_biquad *bq, double x) {
    double y = bq->b0 * x + bq->b1 * bq->x1 + bq->b2 * bq->x2
             - bq->a1 * bq->y1 - bq->a2 * bq->y2;
    bq->x2 = bq->x1; bq->x1 = x;
    bq->y2 = bq->y1; bq->y1 = y;
    return y;
}

static inline double snd_filter_step(mac_snd_filter *f, double x) {
    if (!f->enabled) return x;
    /* HP first (kills DC) then LP (anti-alias). Order doesn't change
     * the magnitude response for LTI series but the HP-first ordering
     * means the LP never sees a long sustained DC bias, so its
     * transient settles to silence rather than to the bias level. */
    x = biquad_step(&f->hp, x);
    x = biquad_step(&f->lp, x);
    return x;
}

void mac_snd_extract_vbl(mac_mem *m, u8 *out) {
    if (!m || !m->ram_size || !m->ram || !out) {
        for (unsigned i = 0; i < MAC_SND_SAMPLES_PER_VBL; i++) out[i] = 0x80;
        return;
    }
    /* VIA PB7 = vSndEnb (active low). When set, the PWM output is
     * suppressed by the analog mixer regardless of buffer contents.
     * Mac Plus boot ROM keeps it high through the RAM test, then
     * clears it to play the chime — without this gate the user hears
     * the memory-test patterns as ~20 s of PWM buzz. */
    bool snd_enabled = (m->via.orb & 0x80u) == 0;
    if (!snd_enabled) {
        /* Feed silence (centred sample) through the filter so its
         * state decays cleanly rather than holding the previous
         * window's value when we re-enable. */
        for (unsigned i = 0; i < MAC_SND_SAMPLES_PER_VBL; i++) {
            double s = snd_filter_step(&m->snd_filter, 0.0);
            int q = (int)(s + 128.5);
            if (q < 0) q = 0; else if (q > 255) q = 255;
            out[i] = (u8)q;
        }
        return;
    }
    /* PA3 set = main buffer, clear = alt. The Mac ROM and Sound Manager
     * default to the main buffer; the alternate is for double-buffered
     * playback. */
    bool use_main = (m->via.ora & 0x08u) != 0;
    u32 buf_off = use_main ? 0x0300u : 0x5F00u;
    if (buf_off > m->ram_size) {
        for (unsigned i = 0; i < MAC_SND_SAMPLES_PER_VBL; i++) out[i] = 0x80;
        return;
    }
    u32 base = m->ram_size - buf_off;
    /* Samples are at even byte offsets; the odd bytes are the floppy
     * speed control stream which we ignore. */
    for (unsigned i = 0; i < MAC_SND_SAMPLES_PER_VBL; i++) {
        u32 a = base + i * 2u;
        u8  raw = (a < m->ram_size) ? m->ram[a] : 0x80;
        /* DC-centre the unsigned-8 sample to ±127 for the filter, then
         * re-bias and saturate back to [0..255]. */
        double s = (double)raw - 128.0;
        s = snd_filter_step(&m->snd_filter, s);
        int q = (int)(s + 128.5);     /* round + bias */
        if (q < 0)   q = 0;
        if (q > 255) q = 255;
        out[i] = (u8)q;
    }
}
