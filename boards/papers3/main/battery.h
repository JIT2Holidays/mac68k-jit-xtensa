/* Battery voltage + charging status for the M5Stack PaperS3.
 *
 * Voltage: the battery feeds a ½× resistor divider into GPIO3 (ADC1_CH2);
 * battery_read_mv() undoes the divider and returns the pack voltage in mV.
 * Charging: GPIO4 is the charger status line — it reads LOW while charging
 * (per M5Unified's board_M5PaperS3). Both are sampled at most ~once per second
 * and cached, so they are cheap to poll every rendered frame. */

#ifndef BATTERY_H
#define BATTERY_H

#include <stdbool.h>
#include <stdint.h>

/* GPIO pin map (overridable from the build). */
#ifndef BOARD_BAT_ADC_GPIO
#define BOARD_BAT_ADC_GPIO 3      /* ADC1_CH2 — ½× battery divider */
#endif
#ifndef BOARD_CHG_STAT_GPIO
#define BOARD_CHG_STAT_GPIO 4     /* charger status: low = charging */
#endif

/* Initialize the ADC + the charge-status GPIO. Non-fatal on failure. */
void battery_init(void);

/* Battery voltage in mV (cached ~1 Hz). 0 if not initialized / unavailable. */
uint32_t battery_read_mv(void);

/* True while charging (GPIO4 low), cached ~1 Hz. */
bool battery_is_charging(void);

#endif /* BATTERY_H */
