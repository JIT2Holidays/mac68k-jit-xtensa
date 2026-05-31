/* GT911 capacitive touch -> Macintosh mouse — see touch.h.
 *
 * GT911 low-level access (I2C bus + register layout) follows the paperboy
 * reference; on top of it this turns the panel into a relative track-pad and a
 * bottom-left button circle that drive the emulated Mac mouse. */

#include "touch.h"
#include "board_papers3.h"
#include "mac_mem.h"
#include "mac_input.h"

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "touch";

#define GT911_PORT      I2C_NUM_1
#define GT911_FREQ_HZ   400000
#define GT911_XFER_MS   100
#define GT911_REG_STATUS 0x814E
#define GT911_REG_POINT1 0x814F          /* 8 bytes per point */
static const uint8_t GT911_ADDRS[] = { 0x5D, 0x14 };

/* Visible canvas + Mac screen geometry. */
#define VIS_W 540
#define VIS_H 960
#define MAC_W MAC_SCREEN_W
#define MAC_H MAC_SCREEN_H

/* --- GT911 driver state ------------------------------------------------ */
static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_dev;
static bool s_ready;

/* --- shared mouse state (core 1 writes, core 0 reads) ------------------ */
static volatile int  s_mx = MAC_W / 2;
static volatile int  s_my = MAC_H / 2;
static volatile bool s_btn;

static esp_err_t gt911_read(uint16_t reg, uint8_t *out, size_t len) {
    const uint8_t rb[2] = { (uint8_t)(reg >> 8), (uint8_t)(reg & 0xFF) };
    return i2c_master_transmit_receive(s_dev, rb, 2, out, len, GT911_XFER_MS);
}
static esp_err_t gt911_write(uint16_t reg, uint8_t val) {
    const uint8_t b[3] = { (uint8_t)(reg >> 8), (uint8_t)(reg & 0xFF), val };
    return i2c_master_transmit(s_dev, b, sizeof b, GT911_XFER_MS);
}

static bool gt911_init(void) {
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << BOARD_TOUCH_INT,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,   /* INT is open-drain, active low */
    };
    gpio_config(&io);

    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = GT911_PORT,
        .sda_io_num = BOARD_TOUCH_SDA,
        .scl_io_num = BOARD_TOUCH_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = false,   /* PCB has external pull-ups */
    };
    if (i2c_new_master_bus(&bus_cfg, &s_bus) != ESP_OK) {
        ESP_LOGE(TAG, "i2c bus init failed");
        return false;
    }
    uint8_t addr = 0;
    for (size_t i = 0; i < sizeof GT911_ADDRS; i++)
        if (i2c_master_probe(s_bus, GT911_ADDRS[i], 50) == ESP_OK) { addr = GT911_ADDRS[i]; break; }
    if (!addr) {
        ESP_LOGW(TAG, "GT911 not found (SDA=%d SCL=%d) — mouse disabled",
                 BOARD_TOUCH_SDA, BOARD_TOUCH_SCL);
        i2c_del_master_bus(s_bus);
        return false;
    }
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr,
        .scl_speed_hz = GT911_FREQ_HZ,
    };
    if (i2c_master_bus_add_device(s_bus, &dev_cfg, &s_dev) != ESP_OK) {
        i2c_del_master_bus(s_bus);
        return false;
    }
    ESP_LOGI(TAG, "GT911 at 0x%02X (INT=%d); panel = trackpad, button circle bottom-left",
             addr, BOARD_TOUCH_INT);
    return true;
}

/* Read the current touch points into visible-canvas coords. Returns the count
 * (0..5), or -1 if no fresh sample is available (INT high). */
static int gt911_points(int *vx, int *vy, int max) {
    if (gpio_get_level(BOARD_TOUCH_INT) != 0) return -1;   /* no new data */

    uint8_t status = 0;
    if (gt911_read(GT911_REG_STATUS, &status, 1) != ESP_OK) return -1;
    if (!(status & 0x80)) { gt911_write(GT911_REG_STATUS, 0); return -1; }

    int n = status & 0x0F;
    if (n > 5) n = 5;
    if (n > 0) {
        uint8_t pts[5 * 8];
        if (gt911_read(GT911_REG_POINT1, pts, (size_t)n * 8) != ESP_OK) {
            gt911_write(GT911_REG_STATUS, 0);
            return -1;
        }
        int out = 0;
        for (int p = 0; p < n && out < max; p++) {
            const uint8_t *t = &pts[p * 8];
            int rx = (int)t[1] | ((int)(t[2] & 0x0F) << 8);   /* 0..539 */
            int ry = (int)t[3] | ((int)(t[4] & 0x0F) << 8);   /* 0..959 */
            vx[out] = BOARD_TOUCH_FLIP_X ? (VIS_W - 1 - rx) : rx;
            vy[out] = BOARD_TOUCH_FLIP_Y ? (VIS_H - 1 - ry) : ry;
            out++;
        }
        gt911_write(GT911_REG_STATUS, 0);
        return out;
    }
    gt911_write(GT911_REG_STATUS, 0);
    return 0;   /* fresh sample, finger lifted */
}

static inline bool in_button(int x, int y) {
    int dx = x - BOARD_BTN_CX, dy = y - BOARD_BTN_CY;
    return dx * dx + dy * dy <= BOARD_BTN_R * BOARD_BTN_R;
}

static void touch_task(void *arg) {
    (void)arg;
    if (!gt911_init()) vTaskDelete(NULL);

    float cx = MAC_W / 2.0f, cy = MAC_H / 2.0f;
    const float sens = BOARD_TOUCH_SENS_X10 / 10.0f;
    bool have_pad = false;
    int prev_px = 0, prev_py = 0;

    for (;;) {
        int vx[5], vy[5];
        int n = gt911_points(vx, vy, 5);

        if (n >= 0) {                 /* fresh sample (n==0 means lifted) */
            bool btn = false;
            int pad = -1;
            for (int i = 0; i < n; i++) {
                if (in_button(vx[i], vy[i])) btn = true;
                else if (pad < 0) pad = i;   /* first pad finger drives motion */
            }
            if (pad >= 0) {
                if (have_pad) {
                    cx += (vx[pad] - prev_px) * sens;
                    cy += (vy[pad] - prev_py) * sens;
                    if (cx < 0) cx = 0;
                    if (cx > MAC_W - 1) cx = MAC_W - 1;
                    if (cy < 0) cy = 0;
                    if (cy > MAC_H - 1) cy = MAC_H - 1;
                }
                prev_px = vx[pad];
                prev_py = vy[pad];
                have_pad = true;
            } else {
                have_pad = false;     /* no pad finger — drop the motion anchor */
            }
            s_mx = (int)(cx + 0.5f);
            s_my = (int)(cy + 0.5f);
            s_btn = btn;
        }
        /* n < 0: no fresh sample (finger held between reports) — keep state. */

        vTaskDelay(pdMS_TO_TICKS(15));   /* ~66 Hz poll */
    }
}

void touch_start(void) {
    /* Below the e-ink task (EINK_TASK_PRIO), which is the highest application
     * priority so it can hold a steady 30 fps. The panel task yields its
     * inter-frame slack (~9 ms of every 33 ms) via vTaskDelayUntil, so this
     * 66 Hz poll still runs promptly between fields. */
    xTaskCreatePinnedToCore(touch_task, "touch", 4096, NULL, 6, NULL, 1);
}

void touch_apply_mouse(mac_mem *m) {
    mac_set_mouse(m, s_mx, s_my, s_btn);
}
