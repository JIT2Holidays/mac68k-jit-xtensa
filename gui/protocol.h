#ifndef MAC_GUI_PROTOCOL_H
#define MAC_GUI_PROTOCOL_H

/* Wire protocol between the SDL GUI frontend and the emulator backend.
 *
 * The backend may be the host emulator (`mac68k_host --server`, talking
 * over a pipe) or the ESP32-S3 firmware running under qemu-system-xtensa
 * (talking over the emulated UART). Either way it is a plain byte stream;
 * this header is shared by both ends.
 *
 * Every packet is a 4-byte header followed by `len` payload bytes:
 *   [type:u8][len: 3 bytes little-endian]
 *
 * Backend -> GUI:
 *   'H'  hello   payload = {u16 width, u16 height}  (little-endian)
 *   'F'  frame   payload = RLE-encoded 1bpp framebuffer (width/8 * height)
 *
 * GUI -> backend:
 *   'M'  mouse   payload = {i16 x, i16 y, u8 buttons}
 *   'K'  key     payload = {u8 mac_keycode, u8 down}
 *   'R'  reset   payload = none
 *
 * The framebuffer is the raw Macintosh 1-bit screen (a set bit = a black
 * pixel). RLE keeps the UART backend tractable — a mostly-static Mac
 * screen compresses to a few KB. */

#include <stdint.h>
#include <stddef.h>

#define MACGUI_SCREEN_W   512
#define MACGUI_SCREEN_H   342
#define MACGUI_FB_BYTES   ((MACGUI_SCREEN_W / 8) * MACGUI_SCREEN_H)

#define MACGUI_PKT_HELLO  'H'
#define MACGUI_PKT_FRAME  'F'
#define MACGUI_PKT_MOUSE  'M'
#define MACGUI_PKT_KEY    'K'
#define MACGUI_PKT_RESET  'R'

/* RLE-encode `n` bytes of `src` into `dst`; returns the encoded length.
 * `dst` must hold at least 2*n bytes (worst case). Encoding is a stream
 * of [count:1][value:1] pairs, count 1..255. */
static inline size_t macgui_rle_encode(const uint8_t *src, size_t n,
                                       uint8_t *dst) {
    size_t i = 0, o = 0;
    while (i < n) {
        uint8_t v = src[i];
        size_t run = 1;
        while (i + run < n && src[i + run] == v && run < 255) run++;
        dst[o++] = (uint8_t)run;
        dst[o++] = v;
        i += run;
    }
    return o;
}

/* Decode an RLE stream of `n` bytes into `dst` (capacity `cap`).
 * Returns the number of bytes produced. */
static inline size_t macgui_rle_decode(const uint8_t *src, size_t n,
                                       uint8_t *dst, size_t cap) {
    size_t i = 0, o = 0;
    while (i + 1 < n && o < cap) {
        uint8_t run = src[i++], v = src[i++];
        while (run-- && o < cap) dst[o++] = v;
    }
    return o;
}

#endif
