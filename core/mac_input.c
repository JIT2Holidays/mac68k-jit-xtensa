/* Macintosh mouse and keyboard input.
 *
 * Mouse: the host pointer is written into the Mac's low-memory mouse
 * globals, the way mini vMac's MOUSEMDV.c does it.
 *
 * Keyboard: the Mac Plus keyboard is a serial device clocked through the
 * VIA shift register. The state machine is ported from mini vMac's
 * KBRDEMDV.c — the guest writes a command into the VIA SR, the keyboard
 * decodes it and shifts a reply back, raising the SR interrupt. */

#include "mac_input.h"
#include "mac_mem.h"
#include <string.h>

/* --- module state ------------------------------------------------------ */

enum { KBD_IDLE, KBD_RX_CMD, KBD_GOT_CMD, KBD_RX_END };

static struct {
    mac_mem *vm;

    /* keyboard state machine */
    int  state;
    bool have_result;
    u8   result;
    u8   instant_cmd;
    bool inquiry_pending;
    u64  inquiry_deadline;

    /* pending timed action: 0 none, 1 receive-command, 2 receive-end */
    int  timed_action;
    u64  timed_deadline;

    /* key-event queue */
    struct { u8 code; u8 down; } keyq[64];
    int  kq_head, kq_tail;
} K;

/* --- mouse ------------------------------------------------------------- */

/* Mac low-memory mouse globals. */
#define LM_MTemp    0x0828   /* interim mouse location  */
#define LM_RawMouse 0x082C   /* raw mouse location      */
#define LM_Mouse    0x0830   /* processed mouse location */
#define LM_CrsrNew  0x08CE
#define LM_CrsrCouple 0x08CF

void mac_input_init(mac_mem *m) {
    memset(&K, 0, sizeof(K));
    K.vm = m;
    K.instant_cmd = 0x7B;
    K.state = KBD_IDLE;
}

void mac_set_mouse(mac_mem *m, int x, int y, bool button_down) {
    /* Hold off until the OS has enabled SCC interrupts — until then the
     * mouse low-memory globals are not set up and writing them corrupts
     * the still-booting system (mini vMac gates the mouse the same way). */
    if (!mac_mouse_enabled(m)) return;

    if (x < 0) x = 0; if (x > MAC_SCREEN_W - 1) x = MAC_SCREEN_W - 1;
    if (y < 0) y = 0; if (y > MAC_SCREEN_H - 1) y = MAC_SCREEN_H - 1;

    u32 pos = ((u32)y << 16) | (u32)(x & 0xFFFF);
    if (mac_read32(m, LM_MTemp) != pos) {
        mac_write32(m, LM_MTemp, pos);
        mac_write32(m, LM_RawMouse, pos);
        mac_write8 (m, LM_CrsrNew, mac_read8(m, LM_CrsrCouple));
    }
    /* VIA PB3: 0 = button pressed, 1 = up. */
    m->mouse_btn = button_down ? 0 : 1;
}

/* --- keyboard event queue --------------------------------------------- */

void mac_key_event(mac_mem *m, int mac_keycode, bool down) {
    (void)m;
    int next = (K.kq_tail + 1) & 63;
    if (next == K.kq_head) return;            /* queue full — drop */
    K.keyq[K.kq_tail].code = (u8)(mac_keycode & 0x7F);
    K.keyq[K.kq_tail].down = down ? 1 : 0;
    K.kq_tail = next;
}

static bool find_key_event(int *code, bool *down) {
    if (K.kq_head == K.kq_tail) return false;
    *code = K.keyq[K.kq_head].code;
    *down = K.keyq[K.kq_head].down != 0;
    K.kq_head = (K.kq_head + 1) & 63;
    return true;
}

/* --- VIA shift-register glue ------------------------------------------ */

/* ACR shift-mode field. Mode 6 = shift out under O2 clock (the Mac uses
 * this to pull CB2 low); mode 7 = shift out under external clock;
 * mode 3 = shift in under external clock. */
static int shift_mode(mac_mem *m) { return (m->via.acr & 0x1C) >> 2; }

static void via_set_irq(mac_mem *m, u8 bits) {
    m->via.ifr |= bits;
    mac_via_recalc_irq(m);
}

/* The keyboard reads the byte the guest shifted out (its command). */
static u8 via_shift_out_data(mac_mem *m) {
    via_set_irq(m, VIA_IRQ_SR | VIA_IRQ_CB1);
    m->via.icb2 = m->via.sr & 1;
    return m->via.sr;
}

/* The keyboard shifts a reply byte back into the SR. */
static void via_shift_in_data(mac_mem *m, u8 v) {
    m->via.sr = v;
    via_set_irq(m, VIA_IRQ_SR | VIA_IRQ_CB1);
}

/* --- keyboard state machine (ported from KBRDEMDV.c) ------------------ */

#define KBD_DELAY_CYCLES 8000        /* ~ one keyboard byte time */
#define KBD_INQUIRY_CYCLES 2000000   /* ~ 0.25 s inquiry timeout */

static void got_keyboard_data(mac_mem *m, u8 v) {
    if (K.state != KBD_IDLE) {
        K.have_result = true;
        K.result = v;
    } else {
        via_shift_in_data(m, v);
        m->via.icb2 = 1;
    }
}

static bool attempt_finish_inquiry(mac_mem *m) {
    int code; bool down;
    if (!find_key_event(&code, &down)) return false;
    u8 data;
    if (code < 64) {
        data = (u8)(code << 1);
        if (!down) data += 128;
    } else {
        data = 121;
        K.instant_cmd = (u8)((code - 64) << 1);
        if (!down) K.instant_cmd += 128;
    }
    got_keyboard_data(m, data);
    return true;
}

static void do_receive_command(mac_mem *m) {
    u8 in = via_shift_out_data(m);
    K.state = KBD_GOT_CMD;
    switch (in) {
        case 0x10:                     /* Inquiry */
            if (!attempt_finish_inquiry(m)) {
                K.inquiry_pending = true;
                K.inquiry_deadline = m->cpu->cycles + KBD_INQUIRY_CYCLES;
            }
            break;
        case 0x14:                     /* Instant */
            got_keyboard_data(m, K.instant_cmd);
            K.instant_cmd = 0x7B;
            break;
        case 0x16:                     /* Model */
            got_keyboard_data(m, 0x0B);
            break;
        case 0x36:                     /* Test */
            got_keyboard_data(m, 0x7D);
            break;
        default:
            got_keyboard_data(m, 0);
            break;
    }
}

static void do_receive_end_command(mac_mem *m) {
    K.state = KBD_IDLE;
    if (K.have_result) {
        K.have_result = false;
        via_shift_in_data(m, K.result);
        m->via.icb2 = 1;
    }
}

/* Called whenever the CB2 (keyboard data) line changes state. */
static void kbd_cb2_changed(mac_mem *m) {
    switch (K.state) {
        case KBD_IDLE:
            if (m->via.icb2 == 0) {
                K.state = KBD_RX_CMD;
                K.timed_action = 1;
                K.timed_deadline = m->cpu->cycles + KBD_DELAY_CYCLES;
                K.inquiry_pending = false;
            }
            break;
        case KBD_GOT_CMD:
            if (m->via.icb2 == 1) {
                K.state = KBD_RX_END;
                K.timed_action = 2;
                K.timed_deadline = m->cpu->cycles + KBD_DELAY_CYCLES;
            }
            break;
    }
}

/* --- hooks called from mac_mem.c -------------------------------------- */

void mac_kbd_sr_written(mac_mem *m) {
    m->via.ifr &= (u8)~VIA_IRQ_SR;
    mac_via_recalc_irq(m);
    /* Shift mode 6 with SR == 0 pulls CB2 low — the keyboard "start". */
    if (shift_mode(m) == 6 && m->via.sr == 0) {
        if (m->via.icb2 != 0) { m->via.icb2 = 0; kbd_cb2_changed(m); }
    }
}

void mac_kbd_acr_written(mac_mem *m, u8 old_acr) {
    /* When the shift register stops being an output, CB2 floats high. */
    if ((old_acr & 0x10) != (m->via.acr & 0x10) && (m->via.acr & 0x10) == 0) {
        if (m->via.icb2 == 0) { m->via.icb2 = 1; kbd_cb2_changed(m); }
    }
}

void mac_kbd_tick(mac_mem *m, u64 cycles) {
    if (K.timed_action != 0 && cycles >= K.timed_deadline) {
        int act = K.timed_action;
        K.timed_action = 0;
        if (act == 1) do_receive_command(m);
        else          do_receive_end_command(m);
    }
    /* Pending Inquiry — deliver a queued key as soon as one is available,
     * else send a null reply once the timeout elapses (the ROM resets the
     * keyboard if it goes quiet too long). */
    if (K.inquiry_pending) {
        if (attempt_finish_inquiry(m)) {
            K.inquiry_pending = false;
        } else if (cycles >= K.inquiry_deadline) {
            K.inquiry_pending = false;
            got_keyboard_data(m, 0x7B);
        }
    }
}
