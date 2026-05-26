/* Mac Plus sound: extract 8-bit PWM samples from the VIA-selected
 * sound buffer at the top of RAM. See mac_snd.h for the layout. */

#include "mac_snd.h"
#include "mac_mem.h"

void mac_snd_extract_vbl(mac_mem *m, u8 *out) {
    if (!m || !m->ram_size || !m->ram || !out) {
        for (unsigned i = 0; i < MAC_SND_SAMPLES_PER_VBL; i++) out[i] = 0x80;
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
        out[i] = (a < m->ram_size) ? m->ram[a] : 0x80;
    }
}
