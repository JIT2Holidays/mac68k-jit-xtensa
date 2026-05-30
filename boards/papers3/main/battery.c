/* Battery voltage + charging status — see battery.h.
 *
 * Ported from the paperboy reference's battery driver (GPIO3 / ADC1_CH2, ½×
 * divider, curve-fitting calibration), plus the charger status line on GPIO4
 * (low = charging) as M5Unified reads it for the PaperS3. */

#include "battery.h"

#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_timer.h"
#include "esp_log.h"

static const char *TAG = "battery";

#define BAT_ADC_UNIT     ADC_UNIT_1
#define BAT_ADC_CHANNEL  ADC_CHANNEL_2     /* GPIO3 on ESP32-S3 */
#define BAT_ADC_ATTEN    ADC_ATTEN_DB_12   /* ~0–2.5 V at the pin -> 0–5 V batt */
#define BAT_SAMPLES      8
#define BAT_DIVIDER      2                 /* ½× divider */
#define BAT_SAMPLE_US    1000000LL         /* re-sample at most once per second */

static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t          s_cali;
static bool   s_cali_valid;
static bool   s_ready;

static int64_t  s_next_us;
static uint32_t s_mv;
static bool     s_charging;

void battery_init(void) {
    adc_oneshot_unit_init_cfg_t unit_cfg = { .unit_id = BAT_ADC_UNIT };
    if (adc_oneshot_new_unit(&unit_cfg, &s_adc) != ESP_OK) {
        ESP_LOGE(TAG, "ADC unit init failed");
        return;
    }
    adc_oneshot_chan_cfg_t chan_cfg = {
        .atten = BAT_ADC_ATTEN,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    if (adc_oneshot_config_channel(s_adc, BAT_ADC_CHANNEL, &chan_cfg) != ESP_OK) {
        ESP_LOGE(TAG, "ADC channel config failed");
        return;
    }
    adc_cali_curve_fitting_config_t cali_cfg = {
        .unit_id = BAT_ADC_UNIT,
        .atten = BAT_ADC_ATTEN,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    s_cali_valid = (adc_cali_create_scheme_curve_fitting(&cali_cfg, &s_cali) == ESP_OK);

    /* Charger status input (low = charging). */
    gpio_config_t io = {
        .mode = GPIO_MODE_INPUT,
        .pin_bit_mask = 1ULL << BOARD_CHG_STAT_GPIO,
    };
    gpio_config(&io);

    s_ready = true;
    s_next_us = 0;
    ESP_LOGI(TAG, "battery on GPIO%d (ADC1_CH2), charge status on GPIO%d (low=charging)%s",
             BOARD_BAT_ADC_GPIO, BOARD_CHG_STAT_GPIO,
             s_cali_valid ? "" : " [uncalibrated]");
}

/* Refresh the cached voltage + charge state if the sample interval elapsed. */
static void sample(void) {
    if (!s_ready) return;
    int64_t now = esp_timer_get_time();
    if (now < s_next_us) return;
    s_next_us = now + BAT_SAMPLE_US;

    int sum = 0;
    for (int i = 0; i < BAT_SAMPLES; i++) {
        int raw = 0;
        adc_oneshot_read(s_adc, BAT_ADC_CHANNEL, &raw);
        sum += raw;
    }
    int avg = sum / BAT_SAMPLES;
    int mv_pin = 0;
    if (s_cali_valid) {
        adc_cali_raw_to_voltage(s_cali, avg, &mv_pin);
    } else {
        mv_pin = (int)((int64_t)avg * 2500 / 4095);   /* rough fallback */
    }
    s_mv = (uint32_t)(mv_pin * BAT_DIVIDER);
    s_charging = (gpio_get_level(BOARD_CHG_STAT_GPIO) == 0);
}

uint32_t battery_read_mv(void) {
    sample();
    return s_mv;
}

bool battery_is_charging(void) {
    sample();
    return s_charging;
}
