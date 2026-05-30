/* USB-Serial-JTAG remote control — see uart_ctl.h. */

#include "uart_ctl.h"
#include "mac_input.h"
#include <string.h>
#include <stdio.h>

#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "freertos/FreeRTOS.h"

#define FB_W      512
#define FB_H      342
#define FB_BYTES  ((FB_W / 8) * FB_H)   /* 21888 */

/* Click timing in guest cycles (7.8336 MHz). ~38 ms hold / gap keeps both
 * presses of a double-click well inside the Mac's default double-click
 * window while giving the VBL mouse sampler several edges to see. */
#define CLK_STEP  200000u

void uart_ctl_init(void) {
    usb_serial_jtag_driver_config_t cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    cfg.rx_buffer_size = 1024;
    cfg.tx_buffer_size = 2048;
    usb_serial_jtag_driver_install(&cfg);
    usb_serial_jtag_vfs_use_driver();   /* stdio (ESP_LOG/printf) via driver */
}

/* Pending timed events. kind 0 = mouse(x,y,btn), 1 = key(code,down). */
typedef struct { u64 cyc; u8 kind; i16 x, y; u8 a, b; } ev_t;
static ev_t s_ev[16];
static int  s_nev;
static char s_line[80];
static int  s_ll;

bool uart_ctl_busy(void) { return s_nev > 0; }

static void sched(u64 cyc, u8 kind, i16 x, i16 y, u8 a, u8 b) {
    if (s_nev < (int)(sizeof(s_ev) / sizeof(s_ev[0])))
        s_ev[s_nev++] = (ev_t){ cyc, kind, x, y, a, b };
}

static const char B64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static void dump_fb(mac_mem *m) {
    char hdr[40];
    int hl = snprintf(hdr, sizeof hdr, "\nFB:%d,%d,%d\n", FB_W, FB_H, FB_BYTES);
    usb_serial_jtag_write_bytes(hdr, hl, portMAX_DELAY);
    u32 fb = m->fb_base, rs = m->ram_size;
    char buf[256]; int bl = 0;
    for (u32 i = 0; i < FB_BYTES; i += 3) {
        u8 b0 = m->ram[(fb + i) % rs];
        u8 b1 = (i + 1 < FB_BYTES) ? m->ram[(fb + i + 1) % rs] : 0;
        u8 b2 = (i + 2 < FB_BYTES) ? m->ram[(fb + i + 2) % rs] : 0;
        buf[bl++] = B64[b0 >> 2];
        buf[bl++] = B64[((b0 & 3) << 4) | (b1 >> 4)];
        buf[bl++] = (i + 1 < FB_BYTES) ? B64[((b1 & 15) << 2) | (b2 >> 6)] : '=';
        buf[bl++] = (i + 2 < FB_BYTES) ? B64[b2 & 63] : '=';
        if (bl >= 252) { usb_serial_jtag_write_bytes(buf, bl, portMAX_DELAY); bl = 0; }
    }
    if (bl) usb_serial_jtag_write_bytes(buf, bl, portMAX_DELAY);
    usb_serial_jtag_write_bytes("\n:FBEND\n", 8, portMAX_DELAY);
}

static void exec_cmd(m68k_cpu *cpu, mac_mem *m, char *line) {
    u64 now = cpu->cycles;
    int x, y, a, b;
    if (line[0] == 'd' && line[1] == 0) {
        dump_fb(m);
    } else if (sscanf(line, "m %d %d", &x, &y) == 2) {
        mac_set_mouse(m, (i16)x, (i16)y, false);
    } else if (sscanf(line, "b %d %d %d", &x, &y, &a) == 3) {
        mac_set_mouse(m, (i16)x, (i16)y, a != 0);
    } else if (sscanf(line, "c %d %d", &x, &y) == 2) {
        sched(now,             0, x, y, 0, 1);
        sched(now + CLK_STEP,  0, x, y, 0, 0);
    } else if (sscanf(line, "C %d %d", &x, &y) >= 2) {
        u32 s = CLK_STEP;
        int st; if (sscanf(line, "C %d %d %d", &x, &y, &st) == 3 && st > 0) s = (u32)st;
        sched(now,             0, x, y, 0, 1);
        sched(now + s,         0, x, y, 0, 0);
        sched(now + 2 * s,     0, x, y, 0, 1);
        sched(now + 3 * s,     0, x, y, 0, 0);
    } else if (sscanf(line, "k %d %d", &a, &b) == 2) {
        mac_key_event(m, (u8)a, b != 0);
    } else if (sscanf(line, "K %d", &a) == 1) {
        sched(now,            1, 0, 0, (u8)a, 1);
        sched(now + CLK_STEP, 1, 0, 0, (u8)a, 0);
    }
    printf("OK\n");
}

void uart_ctl_poll(m68k_cpu *cpu, mac_mem *m) {
    /* Apply now-due timed events (keep the rest). */
    int w = 0;
    for (int i = 0; i < s_nev; i++) {
        if (s_ev[i].cyc <= cpu->cycles) {
            if (s_ev[i].kind == 0)
                mac_set_mouse(m, s_ev[i].x, s_ev[i].y, s_ev[i].b != 0);
            else
                mac_key_event(m, s_ev[i].a, s_ev[i].b != 0);
        } else {
            s_ev[w++] = s_ev[i];
        }
    }
    s_nev = w;

    /* Drain pending command bytes (non-blocking). */
    uint8_t rx[64];
    int n = usb_serial_jtag_read_bytes(rx, sizeof rx, 0);
    for (int i = 0; i < n; i++) {
        char ch = (char)rx[i];
        if (ch == '\r') continue;
        if (ch == '\n') {
            s_line[s_ll] = 0;
            if (s_ll) exec_cmd(cpu, m, s_line);
            s_ll = 0;
        } else if (s_ll < (int)sizeof(s_line) - 1) {
            s_line[s_ll++] = ch;
        }
    }
}
